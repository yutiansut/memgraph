#pragma once

#include "communication/rpc/serialization.hpp"
#include "communication/rpc/streams.hpp"
#include "storage/common/types/property_value.hpp"
#include "storage/common/types/property_value_store.hpp"
#include "storage/common/types/types.hpp"

namespace slk {

inline void Save(const storage::Label &common, slk::Builder *builder) {
  slk::Save(common.id_, builder);
}

inline void Load(storage::Label *common, slk::Reader *reader) {
  slk::Load(&common->id_, reader);
}

inline void Save(const storage::EdgeType &common, slk::Builder *builder) {
  slk::Save(common.id_, builder);
}

inline void Load(storage::EdgeType *common, slk::Reader *reader) {
  slk::Load(&common->id_, reader);
}
inline void Save(const storage::Property &common, slk::Builder *builder) {
  slk::Save(common.id_, builder);
}

inline void Load(storage::Property *common, slk::Reader *reader) {
  slk::Load(&common->id_, reader);
}

void Save(const PropertyValue &value, slk::Builder *builder);

void Load(PropertyValue *value, slk::Reader *reader);

void Save(const PropertyValueStore &properties, slk::Builder *builder);

void Load(PropertyValueStore *properties, slk::Reader *reader);

}  // namespace slk