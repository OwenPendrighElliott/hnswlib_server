// data_store.cpp
#include "data_store.hpp"
#include <fstream>
#include <algorithm>
#include <typeindex>

// VariantComparator implementation
bool VariantComparator::operator()(const FieldValue& lhs, const FieldValue& rhs) const {
    return lhs < rhs;
}

// Template methods for comparison
template<typename T>
bool DataStore::compare(const T& a, const T& b, const std::string& type) {
    if (type == "=") return a == b;
    if (type == "!=") return a != b;
    if (type == ">") return a > b;
    if (type == "<") return a < b;
    if (type == ">=") return a >= b;
    if (type == "<=") return a <= b;
    throw std::runtime_error("Unsupported comparison type");
}

// Template methods for filtering by type
template<typename T>
std::set<int> DataStore::filterByType(const std::map<FieldValue, std::set<int>, VariantComparator>& fieldData, const std::string& type, const FieldValue& value) {
    std::set<int> result;
    for (const auto& [fieldValue, ids] : fieldData) {
        if (auto castedFieldValue = std::get_if<T>(&fieldValue)) {
            if (compare(*castedFieldValue, std::get<T>(value), type)) {
                result.insert(ids.begin(), ids.end());
            }
        }
    }
    return result;
}

// Set a record in the data store
void DataStore::set(int id, std::map<std::string, FieldValue> record) {
    std::lock_guard<std::mutex> lock(mutex);
    data[id] = std::move(record);
    ids.insert(id);
    for (const auto& [field, value] : data[id]) {
        field_index[field][value].insert(id);
    }
}

// Get a record from the data store
std::map<std::string, FieldValue> DataStore::get(int id) {
    return data.at(id);
}

void DataStore::remove(int id) {
    std::lock_guard<std::mutex> lock(mutex);

    if (data.find(id) == data.end()) return;
    auto record = data[id];
    for (const auto& [field, value] : record) {
        field_index[field][value].erase(id);
    }
    data.erase(id);
    ids.erase(id);
}

std::set<int> DataStore::filter(std::shared_ptr<FilterASTNode> filters) {
    std::set<int> result;
    if (filters == nullptr) {
        return result;
    }

    switch (filters->type) {
        case NodeType::Comparison: {
            auto filter = filters->filter;
            auto fieldData = field_index[filter.field];
            if (fieldData.empty()) {
                return result;
            }

            if (std::holds_alternative<long>(filter.value)) {
                result = filterByType<long>(fieldData, filter.type, filter.value);
            } else if (std::holds_alternative<double>(filter.value)) {
                result = filterByType<double>(fieldData, filter.type, filter.value);
            } else if (std::holds_alternative<std::string>(filter.value)) {
                result = filterByType<std::string>(fieldData, filter.type, filter.value);
            }
            break;
        }
        case NodeType::BooleanOp: {
            auto left = filter(filters->left);
            auto right = filter(filters->right);
            if (filters->booleanOp == BooleanOp::And) {
                std::set_intersection(left.begin(), left.end(), right.begin(), right.end(), std::inserter(result, result.begin()));
            } else {
                std::set_union(left.begin(), left.end(), right.begin(), right.end(), std::inserter(result, result.begin()));
            }
            break;
        }
        case NodeType::Not: {
            auto child = filter(filters->child);
            std::set_difference(ids.begin(), ids.end(), child.begin(), child.end(), std::inserter(result, result.begin()));
            break;
        }
    }

    return result;
}


namespace {
    void serializeFieldValue(std::ofstream& outFile, const FieldValue& value) {
        int index = value.index();
        outFile.write(reinterpret_cast<const char*>(&index), sizeof(index));

        switch (index) {
            case 0: // long
                {
                    long v = std::get<long>(value);
                    outFile.write(reinterpret_cast<const char*>(&v), sizeof(v));
                }
                break;
            case 1: // double
                {
                    double v = std::get<double>(value);
                    outFile.write(reinterpret_cast<const char*>(&v), sizeof(v));
                }
                break;
            case 2: // std::string
                {
                    const std::string& str = std::get<std::string>(value);
                    size_t size = str.size();
                    outFile.write(reinterpret_cast<const char*>(&size), sizeof(size));
                    outFile.write(str.data(), size);
                }
                break;
            default:
                throw std::runtime_error("Unknown variant index during serialization");
        }
    }

    FieldValue deserializeFieldValue(std::ifstream& inFile) {
        int index;
        inFile.read(reinterpret_cast<char*>(&index), sizeof(index));

        switch (index) {
            case 0: // long
                {
                    long v;
                    inFile.read(reinterpret_cast<char*>(&v), sizeof(v));
                    return v;
                }
            case 1: // double
                {
                    double v;
                    inFile.read(reinterpret_cast<char*>(&v), sizeof(v));
                    return v;
                }
            case 2: // std::string
                {
                    size_t size;
                    inFile.read(reinterpret_cast<char*>(&size), sizeof(size));
                    std::string str(size, '\0');
                    inFile.read(&str[0], size);
                    return str;
                }
            default:
                throw std::runtime_error("Unknown variant index during deserialization");
        }
    }
}

void DataStore::serialize(const std::string &filename) {
    std::ofstream outFile(filename, std::ios::binary);
    if (!outFile) {
        throw std::runtime_error("Failed to open file for serialization.");
    }

    size_t recordCount = data.size();
    outFile.write(reinterpret_cast<const char*>(&recordCount), sizeof(recordCount));

    for (const auto& [id, record] : data) {
        outFile.write(reinterpret_cast<const char*>(&id), sizeof(id));
        size_t fieldCount = record.size();
        outFile.write(reinterpret_cast<const char*>(&fieldCount), sizeof(fieldCount));

        for (const auto& [field, value] : record) {
            size_t fieldLength = field.size();
            outFile.write(reinterpret_cast<const char*>(&fieldLength), sizeof(fieldLength));
            outFile.write(field.data(), fieldLength);

            // Serialize the FieldValue object
            serializeFieldValue(outFile, value);
        }
    }
}

void DataStore::deserialize(const std::string &filename) {
    // std::lock_guard<std::mutex> lock(mutex);

    std::ifstream inFile(filename, std::ios::binary);
    if (!inFile) {
        throw std::runtime_error("Failed to open file for deserialization.");
    }

    size_t recordCount;
    inFile.read(reinterpret_cast<char*>(&recordCount), sizeof(recordCount));

    for (size_t i = 0; i < recordCount; ++i) {
        int id;
        inFile.read(reinterpret_cast<char*>(&id), sizeof(id));

        size_t fieldCount;
        inFile.read(reinterpret_cast<char*>(&fieldCount), sizeof(fieldCount));

        std::map<std::string, FieldValue> record;
        for (size_t j = 0; j < fieldCount; ++j) {
            size_t fieldLength;
            inFile.read(reinterpret_cast<char*>(&fieldLength), sizeof(fieldLength));

            std::string field(fieldLength, '\0');
            inFile.read(field.data(), fieldLength);

            // Deserialize the FieldValue object
            FieldValue value = deserializeFieldValue(inFile);

            record[field] = value;
            
            // Update the field index
            field_index[field][value].insert(id);
        }

        data[id] = record;
        ids.insert(id);
    }
}