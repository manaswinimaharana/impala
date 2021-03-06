// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef IMPALA_UTIL_DICT_ENCODING_H
#define IMPALA_UTIL_DICT_ENCODING_H

#include <map>

#include <boost/unordered_map.hpp>

#include "common/compiler-util.h"
#include "exec/parquet-common.h"
#include "gutil/strings/substitute.h"
#include "runtime/mem-pool.h"
#include "runtime/string-value.h"
#include "util/bit-util.h"
#include "util/rle-encoding.h"

namespace impala {

/// See the dictionary encoding section of https://github.com/Parquet/parquet-format.
/// This class supports dictionary encoding of all Impala types.
/// The encoding supports streaming encoding. Values are encoded as they are added while
/// the dictionary is being constructed. At any time, the buffered values can be
/// written out with the current dictionary size. More values can then be added to
/// the encoder, including new dictionary entries.
/// TODO: if the dictionary was made to be ordered, the dictionary would compress better.
/// Add this to the spec as future improvement.

/// Base class for encoders. This is convenient so users can have a type that
/// abstracts over the actual dictionary type.
/// Note: it does not provide a virtual Put(). Users are expected to know the subclass
/// type when using Put().
/// TODO: once we can easily remove virtual calls with codegen, this interface can
/// rely less on templating and be easier to follow. The type should be passed in
/// as an argument rather than template argument.
class DictEncoderBase {
 public:
  virtual ~DictEncoderBase() {
    DCHECK(buffered_indices_.empty());
  }

  /// Writes out the encoded dictionary to buffer. buffer must be preallocated to
  /// dict_encoded_size() bytes.
  virtual void WriteDict(uint8_t* buffer) = 0;

  /// The number of entries in the dictionary.
  virtual int num_entries() const = 0;

  /// Clears all the indices (but leaves the dictionary).
  void ClearIndices() { buffered_indices_.clear(); }

  /// Returns a conservative estimate of the number of bytes needed to encode the buffered
  /// indices. Used to size the buffer passed to WriteData().
  int EstimatedDataEncodedSize() {
    return 1 + RleEncoder::MaxBufferSize(bit_width(), buffered_indices_.size());
  }

  /// The minimum bit width required to encode the currently buffered indices.
  int bit_width() const {
    if (UNLIKELY(num_entries() == 0)) return 0;
    if (UNLIKELY(num_entries() == 1)) return 1;
    return BitUtil::Log2Ceiling64(num_entries());
  }

  /// Writes out any buffered indices to buffer preceded by the bit width of this data.
  /// Returns the number of bytes written.
  /// If the supplied buffer is not big enough, returns -1.
  /// buffer must be preallocated with buffer_len bytes. Use EstimatedDataEncodedSize()
  /// to size buffer.
  int WriteData(uint8_t* buffer, int buffer_len);

  int dict_encoded_size() { return dict_encoded_size_; }

 protected:
  DictEncoderBase(MemPool* pool)
    : dict_encoded_size_(0), pool_(pool) {
  }

  /// Indices that have not yet be written out by WriteData().
  std::vector<int> buffered_indices_;

  /// The number of bytes needed to encode the dictionary.
  int dict_encoded_size_;

  /// Pool to store StringValue data. Not owned.
  MemPool* pool_;
};

template<typename T>
class DictEncoder : public DictEncoderBase {
 public:
  DictEncoder(MemPool* pool, int encoded_value_size) :
      DictEncoderBase(pool), buckets_(HASH_TABLE_SIZE, Node::INVALID_INDEX),
      encoded_value_size_(encoded_value_size) { }

  /// Encode value. Returns the number of bytes added to the dictionary page length
  /// (will be 0 if this value is already in the dictionary) or -1 if the dictionary is
  /// full (in which case the caller should give up on dictionary encoding). Note that
  /// this does not actually write any data, just buffers the value's index to be
  /// written later.
  int Put(const T& value);

  virtual void WriteDict(uint8_t* buffer);

  virtual int num_entries() const { return nodes_.size(); }

 private:
  /// Size of the table. Must be a power of 2.
  enum { HASH_TABLE_SIZE = 1 << 16 };

  /// Dictates an upper bound on the capacity of the hash table.
  typedef uint16_t NodeIndex;

  /// Hash table mapping value to dictionary index (i.e. the number used to encode this
  /// value in the data). Each table entry is a index into the nodes_ vector (giving the
  /// first node of a chain for this bucket) or Node::INVALID_INDEX for an empty bucket.
  std::vector<NodeIndex> buckets_;

  /// Node in the chained hash table.
  struct Node {
    Node(const T& v, const NodeIndex& n) : value(v), next(n) { }

    /// The dictionary value.
    T value;

    /// Index into nodes_ for the next Node in the chain. INVALID_INDEX indicates end.
    NodeIndex next;

    /// The maximum number of values in the dictionary.  Chosen to be around 60% of
    /// HASH_TABLE_SIZE to limit the expected length of the chains.
    enum { INVALID_INDEX = 40000 };
  };

  /// The nodes of the hash table. Ordered by dictionary index (and so also represents
  /// the reverse mapping from encoded index to value).
  std::vector<Node> nodes_;

  /// Size of each encoded dictionary value. -1 for variable-length types.
  int encoded_value_size_;

  /// Hash function for mapping a value to a bucket.
  inline uint32_t Hash(const T& value) const;

  /// Adds value to the hash table and updates dict_encoded_size_. Returns the
  /// number of bytes added to dict_encoded_size_.
  /// bucket gives a pointer to the location (i.e. chain) to add the value
  /// so that the hash for value doesn't need to be recomputed.
  int AddToTable(const T& value, NodeIndex* bucket);
};

/// Decoder class for dictionary encoded data. This class does not allocate any
/// buffers. The input buffers (dictionary buffer and RLE buffer) must be maintained
/// by the caller and valid as long as this object is.
class DictDecoderBase {
 public:
  /// The rle encoded indices into the dictionary. Returns an error status if the buffer
  /// is too short or the bit_width metadata in the buffer is invalid.
  Status SetData(uint8_t* buffer, int buffer_len) {
    DCHECK_GE(buffer_len, 0);
    if (UNLIKELY(buffer_len == 0)) return Status("Dictionary cannot be 0 bytes");
    uint8_t bit_width = *buffer;
    if (UNLIKELY(bit_width < 0 || bit_width > BatchedBitReader::MAX_BITWIDTH)) {
      return Status(strings::Substitute("Dictionary has invalid or unsupported bit "
          "width: $0", bit_width));
    }
    ++buffer;
    --buffer_len;
    data_decoder_.Reset(buffer, buffer_len, bit_width);
    num_repeats_ = 0;
    num_literal_values_ = 0;
    next_literal_idx_ = 0;
    return Status::OK();
  }

  virtual ~DictDecoderBase() {}

  virtual int num_entries() const = 0;

  /// Reads the dictionary value at the specified index into the buffer provided.
  /// The buffer must be large enough to receive the datatype for this dictionary.
  virtual void GetValue(int index, void* buffer) = 0;

 protected:
  /// Number of decoded values to buffer at a time. A multiple of 32 is chosen to allow
  /// efficient reading in batches from data_decoder_. Increasing the batch size up to
  /// 128 seems to improve performance, but increasing further did not make a noticeable
  /// difference.
  static const int DECODED_BUFFER_SIZE = 128;

  RleBatchDecoder<uint32_t> data_decoder_;

  /// Greater than zero if we've started decoding a repeated run.
  int64_t num_repeats_ = 0;

  /// Greater than zero if we have buffered some literal values.
  int num_literal_values_ = 0;

  /// The index of the next decoded value to return.
  int next_literal_idx_ = 0;
};

template<typename T>
class DictDecoder : public DictDecoderBase {
 public:
  /// Construct empty dictionary.
  DictDecoder() {}

  /// Initialize the decoder with an input buffer containing the dictionary.
  /// 'dict_len' is the byte length of dict_buffer.
  /// For string data, the decoder returns StringValues with data directly from
  /// dict_buffer (i.e. no copies).
  /// fixed_len_size is the size that must be passed to decode fixed-length
  /// dictionary values (values stored using FIXED_LEN_BYTE_ARRAY).
  /// Returns true if the dictionary values were all successfully decoded, or false
  /// if the dictionary was corrupt.
  template<parquet::Type::type PARQUET_TYPE>
  bool Reset(uint8_t* dict_buffer, int dict_len, int fixed_len_size) WARN_UNUSED_RESULT;

  virtual int num_entries() const { return dict_.size(); }

  virtual void GetValue(int index, void* buffer) {
    T* val_ptr = reinterpret_cast<T*>(buffer);
    DCHECK_GE(index, 0);
    DCHECK_LT(index, dict_.size());
    *val_ptr = dict_[index];
  }

  /// Returns the next value.  Returns false if the data is invalid.
  /// For StringValues, this does not make a copy of the data.  Instead,
  /// the string data is from the dictionary buffer passed into the c'tor.
  bool GetNextValue(T* value) WARN_UNUSED_RESULT;

 private:
  std::vector<T> dict_;

  /// Decoded values, buffered to allow caller to consume one-by-one. If in the middle of
  /// a repeated run, the first element is the current dict value. If in a literal run,
  /// this contains 'num_literal_values_' values, with the next value to be returned at
  /// 'next_literal_idx_'.
  T decoded_values_[DECODED_BUFFER_SIZE];

  /// Slow path for GetNextValue() where we need to decode new values. Should not be
  /// inlined everywhere.
  bool DecodeNextValue(T* value);
};

template<typename T>
inline int DictEncoder<T>::Put(const T& value) {
  NodeIndex* bucket = &buckets_[Hash(value) & (HASH_TABLE_SIZE - 1)];
  NodeIndex i = *bucket;
  // Look for the value in the dictionary.
  while (i != Node::INVALID_INDEX) {
    const Node* n = &nodes_[i];
    if (LIKELY(n->value == value)) {
      // Value already in dictionary.
      buffered_indices_.push_back(i);
      return 0;
    }
    i = n->next;
  }
  // Value not found. Add it to the dictionary if there's space.
  i = nodes_.size();
  if (UNLIKELY(i >= Node::INVALID_INDEX)) return -1;
  buffered_indices_.push_back(i);
  return AddToTable(value, bucket);
}

template<typename T>
inline uint32_t DictEncoder<T>::Hash(const T& value) const {
  return HashUtil::Hash(&value, sizeof(value), 0);
}

template<>
inline uint32_t DictEncoder<StringValue>::Hash(const StringValue& value) const {
  return HashUtil::Hash(value.ptr, value.len, 0);
}

template<typename T>
inline int DictEncoder<T>::AddToTable(const T& value, NodeIndex* bucket) {
  DCHECK_GT(encoded_value_size_, 0);
  // Prepend the new node to this bucket's chain.
  nodes_.push_back(Node(value, *bucket));
  *bucket = nodes_.size() - 1;
  dict_encoded_size_ += encoded_value_size_;
  return encoded_value_size_;
}

template<>
inline int DictEncoder<StringValue>::AddToTable(const StringValue& value,
    NodeIndex* bucket) {
  char* ptr_copy = reinterpret_cast<char*>(pool_->Allocate(value.len));
  memcpy(ptr_copy, value.ptr, value.len);
  StringValue sv(ptr_copy, value.len);
  // Prepend the new node to this bucket's chain.
  nodes_.push_back(Node(sv, *bucket));
  *bucket = nodes_.size() - 1;
  int bytes_added = ParquetPlainEncoder::ByteSize(sv);
  dict_encoded_size_ += bytes_added;
  return bytes_added;
}

// Force inlining - GCC does not always inline this into hot loops in Parquet scanner.
template <typename T>
ALWAYS_INLINE inline bool DictDecoder<T>::GetNextValue(T* value) {
  // IMPALA-959: Use memcpy() instead of '=' to set *value: addresses are not always 16
  // byte aligned for Decimal16Values.
  if (num_repeats_ > 0) {
    --num_repeats_;
    memcpy(value, &decoded_values_[0], sizeof(T));
    return true;
  } else if (next_literal_idx_ < num_literal_values_) {
    int idx = next_literal_idx_++;
    memcpy(value, &decoded_values_[idx], sizeof(T));
    return true;
  }
  // No decoded values left - need to decode some more.
  return DecodeNextValue(value);
}

template <typename T>
bool DictDecoder<T>::DecodeNextValue(T* value) {
  // IMPALA-959: Use memcpy() instead of '=' to set *value: addresses are not always 16
  // byte aligned for Decimal16Values.
  uint32_t num_repeats = data_decoder_.NextNumRepeats();
  if (num_repeats > 0) {
    uint32_t idx = data_decoder_.GetRepeatedValue(num_repeats);
    if (UNLIKELY(idx >= dict_.size())) return false;
    memcpy(&decoded_values_[0], &dict_[idx], sizeof(T));
    memcpy(value, &decoded_values_[0], sizeof(T));
    num_repeats_ = num_repeats - 1;
    return true;
  } else {
    uint32_t num_literals = data_decoder_.NextNumLiterals();
    if (UNLIKELY(num_literals == 0)) return false;

    uint32_t num_to_decode = std::min<uint32_t>(num_literals, DECODED_BUFFER_SIZE);
    if (UNLIKELY(!data_decoder_.DecodeLiteralValues(
            num_to_decode, dict_.data(), dict_.size(), &decoded_values_[0]))) {
      return false;
    }
    num_literal_values_ = num_to_decode;
    memcpy(value, &decoded_values_[0], sizeof(T));
    next_literal_idx_ = 1;
    return true;
  }
}

template<typename T>
inline void DictEncoder<T>::WriteDict(uint8_t* buffer) {
  for (const Node& node: nodes_) {
    buffer += ParquetPlainEncoder::Encode(node.value, encoded_value_size_, buffer);
  }
}

inline int DictEncoderBase::WriteData(uint8_t* buffer, int buffer_len) {
  // Write bit width in first byte
  *buffer = bit_width();
  ++buffer;
  --buffer_len;

  RleEncoder encoder(buffer, buffer_len, bit_width());
  for (int index: buffered_indices_) {
    if (!encoder.Put(index)) return -1;
  }
  encoder.Flush();
  return 1 + encoder.len();
}

template<typename T>
template<parquet::Type::type PARQUET_TYPE>
inline bool DictDecoder<T>::Reset(uint8_t* dict_buffer, int dict_len,
    int fixed_len_size) {
  dict_.clear();
  uint8_t* end = dict_buffer + dict_len;
  while (dict_buffer < end) {
    T value;
    int decoded_len = ParquetPlainEncoder::Decode<T, PARQUET_TYPE>(dict_buffer, end,
        fixed_len_size, &value);
    if (UNLIKELY(decoded_len < 0)) return false;
    dict_buffer += decoded_len;
    dict_.push_back(value);
  }
  return true;
}

}
#endif
