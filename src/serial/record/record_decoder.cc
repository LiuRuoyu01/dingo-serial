// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "record_decoder.h"

#include <cstdint>
#include <memory>
#include <vector>
#include <algorithm>

// #include "glog/logging.h"

namespace dingodb {

// TODO cast and decode function not good, optimize on 0.8.0 or later

using CastAndDecodeOrSkipFuncPointer = void (*)(
    const std::shared_ptr<BaseSchema>& schema, Buf& key_buf, Buf& value_buf,
    std::vector<std::any>& record, int record_index, bool skip);

template <typename T>
void CastAndDecodeOrSkip(const std::shared_ptr<BaseSchema>& schema,
                         Buf& key_buf, Buf& value_buf,
                         std::vector<std::any>& record, int record_index,
                         bool skip) {
  auto dingo_schema =
      std::dynamic_pointer_cast<DingoSchema<std::optional<T>>>(schema);
  if (skip) {
    if (schema->IsKey()) {
      dingo_schema->SkipKey(&key_buf);
    } else if (!value_buf.IsEnd()) {
      dingo_schema->SkipValue(&value_buf);
    }
  } else {
    if (schema->IsKey()) {
      record.at(record_index) = dingo_schema->DecodeKey(&key_buf);
    } else {
      if (value_buf.IsEnd()) {
        record.at(record_index) = std::optional<T>(std::nullopt);
      } else {
        record.at(record_index) = dingo_schema->DecodeValue(&value_buf);
      }
    }
  }
}

CastAndDecodeOrSkipFuncPointer cast_and_decode_or_skip_func_ptrs[] = {
    CastAndDecodeOrSkip<bool>,
    CastAndDecodeOrSkip<int32_t>,
    CastAndDecodeOrSkip<float>,
    CastAndDecodeOrSkip<int64_t>,
    CastAndDecodeOrSkip<double>,
    CastAndDecodeOrSkip<std::shared_ptr<std::string>>,
    CastAndDecodeOrSkip<std::shared_ptr<std::vector<bool>>>,
    CastAndDecodeOrSkip<std::shared_ptr<std::vector<int32_t>>>,
    CastAndDecodeOrSkip<std::shared_ptr<std::vector<float>>>,
    CastAndDecodeOrSkip<std::shared_ptr<std::vector<int64_t>>>,
    CastAndDecodeOrSkip<std::shared_ptr<std::vector<double>>>,
    CastAndDecodeOrSkip<std::shared_ptr<std::vector<std::string>>>,
};

RecordDecoderV1::RecordDecoderV1(
    int schema_version,
    std::shared_ptr<std::vector<std::shared_ptr<BaseSchema>>> schemas,
    long common_id) {
  this->le_ = IsLE();
  Init(schema_version, schemas, common_id);
}

RecordDecoderV1::RecordDecoderV1(
    int schema_version,
    std::shared_ptr<std::vector<std::shared_ptr<BaseSchema>>> schemas,
    long common_id, bool le) {
  this->le_ = le;
  Init(schema_version, schemas, common_id);
}

void RecordDecoderV1::Init(
    int schema_version,
    std::shared_ptr<std::vector<std::shared_ptr<BaseSchema>>> schemas,
    long common_id) {
  this->schema_version_ = schema_version;
  FormatSchema(schemas, this->le_);
  this->schemas_ = schemas;
  this->common_id_ = common_id;
}

bool RecordDecoderV1::CheckPrefix(Buf& buf) const {
  // skip name space
  buf.Skip(1);
  return buf.ReadLong() == common_id_;
}

bool RecordDecoderV1::CheckReverseTag(Buf& buf) const {
  if (buf.ReverseRead() <= codec_version_) {
    buf.ReverseSkip(3);
    return true;
  }
  return false;
}

bool RecordDecoderV1::CheckSchemaVersion(Buf& buf) const {
  return buf.ReadInt() <= schema_version_;
}

int GetCodecVersion(Buf& buf) { return buf.ReversePeek(); }

void DecodeOrSkip(const std::shared_ptr<BaseSchema>& schema, Buf& key_buf,
                  Buf& value_buf, std::vector<std::any>& record,
                  int record_index, bool skip) {
  cast_and_decode_or_skip_func_ptrs[static_cast<int>(schema->GetType())](
      schema, key_buf, value_buf, record, record_index, skip);
}

int RecordDecoderV1::Decode(const std::string& key, const std::string& value,
                            std::vector<std::any>& record) {
  Buf key_buf(key, this->le_);
  Buf value_buf(value, this->le_);
  if (!CheckPrefix(key_buf)) {
    //"Wrong Common Id"
    return -1;
  }

  if (!CheckReverseTag(key_buf)) {
    //"Wrong Codec Version"
    return -1;
  }

  if (!CheckSchemaVersion(value_buf)) {
    //"Wrong Schema Version"
    return -1;
  }

  record.resize(schemas_->size());
  for (const auto& bs : *schemas_) {
    if (bs) {
      DecodeOrSkip(bs, key_buf, value_buf, record, bs->GetIndex(), false);
    }
  }

  return 0;
}

int RecordDecoderV1::DecodeKey(const std::string& key,
                               std::vector<std::any>& record /*output*/) {
  Buf key_buf(key, this->le_);

  if (!CheckPrefix(key_buf)) {
    //"Wrong Common Id"
    return -1;
  }

  if (!CheckReverseTag(key_buf)) {
    //"Wrong Codec Version"
    return -1;
  }

  record.resize(schemas_->size());
  int index = 0;
  for (const auto& bs : *schemas_) {
    if (bs && bs->IsKey()) {
      DecodeOrSkip(bs, key_buf, key_buf, record, index, false);
    }
    index++;
  }

  return 0;
}

int RecordDecoderV1::Decode(const KeyValue& key_value,
                            std::vector<std::any>& record) {
  return Decode(*key_value.GetKey(), *key_value.GetValue(), record);
}

inline bool IsSkipOnly(
    const std::vector<std::pair<int32_t, int32_t>>& indexed_mapping_index,
    int32_t& n, int32_t& m, int& record_index) {
  int first = indexed_mapping_index[n].first;
  int second = indexed_mapping_index[n].second;
  // DINGO_LOG(DEBUG) << "(" << first << ", " << second << ", " << m << "," << n
  // << ") ";
  if (first == m++) {
    record_index = second;
    n++;
    return false;
  } else {
    return true;
  }
}

int RecordDecoderV1::Decode(const std::string& key, const std::string& value,
                            const std::vector<int>& column_indexes,
                            std::vector<std::any>& record) {
  Buf key_buf(key, this->le_);
  Buf value_buf(value, this->le_);
  if (!CheckPrefix(key_buf) || !CheckReverseTag(key_buf) ||
      !CheckSchemaVersion(value_buf)) {
    return -1;
  }

  record.resize(column_indexes.size());
  int32_t n = 0;
  int32_t m = 0;

  std::vector<std::pair<int, int>> col_index_mapping;

  // index
  col_index_mapping.reserve(column_indexes.size());
  for (unsigned long i = 0; i < column_indexes.size(); i++) {
    col_index_mapping.push_back(std::make_pair(column_indexes[i], i));
  }

  // sort indexed_mapping_index
  std::sort(col_index_mapping.begin(), col_index_mapping.end());

  // sort end indexed_mapping_index and mapping_index
  // DINGO_LOG(DEBUG) << "indexed_mapping_index: ";
  // for (auto p : col_index_mapping) {
  //   DINGO_LOG(DEBUG) << "(" << p.first << ", " << p.second << ") ";
  // }

  int record_index = 0;
  for (auto& bs : *schemas_) {
    if ((int32_t)column_indexes.size() == n) {
      return 0;
    }
    if (bs) {
      bool is_skip = IsSkipOnly(col_index_mapping, n, m, record_index);
      DecodeOrSkip(bs, key_buf, value_buf, record, record_index, is_skip);
    }
  }
  return 0;
}

int RecordDecoderV1::Decode(const KeyValue& key_value,
                            const std::vector<int>& column_indexes,
                            std::vector<std::any>& record) {
  return Decode(*key_value.GetKey(), *key_value.GetValue(), column_indexes,
                record);
}

}  // namespace dingodb
