#include "crow.h"
#include "hnswlib/hnswlib.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <unordered_map>
#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include "data_store.hpp"
#include "models.hpp"
#include "filters.hpp"

#define DEFAULT_INDEX_SIZE 100000
#define INDEX_GROWTH_FACTOR 2

std::unordered_map<std::string, hnswlib::HierarchicalNSW<float>*> indices;
std::unordered_map<std::string, nlohmann::json> indexSettings;
std::unordered_map<std::string, DataStore*> dataStores;

std::shared_mutex indexMutex;
std::mutex dataStoreMutex;

// Functor to filter results with a set of IDs
class FilterIdsInSet : public hnswlib::BaseFilterFunctor {
    public:
    std::unordered_set<int>& ids;
    FilterIdsInSet(std::unordered_set<int>& ids) : ids(ids) {}
    bool operator()(hnswlib::labeltype label_id) {
        return ids.find(label_id) != ids.end();
    }
};

void remove_index_from_disk(const std::string &indexName) {
    std::filesystem::remove("indices/" + indexName + ".bin");
    std::filesystem::remove("indices/" + indexName + ".json");
    std::filesystem::remove("indices/" + indexName + ".data");
}


void write_index_to_disk(const std::string &indexName) {
    std::filesystem::create_directories("indices");

    auto *index = indices[indexName];
    if (!index) {
        std::cerr << "Error: Index not found: " << indexName << std::endl;
        return;
    }

    try {
        index->saveIndex("indices/" + indexName + ".bin");
    } catch (const std::exception &e) {
        std::cerr << "Error saving index: " << e.what() << std::endl;
        return;
    }

    std::ofstream settings_file("indices/" + indexName + ".json");
    if (!settings_file) {
        std::cerr << "Error: Unable to open settings file for writing: " << indexName << std::endl;
        return;
    }
    settings_file << indexSettings[indexName].dump();
}

void read_index_from_disk(const std::string &indexName) {
    std::ifstream settings_file("indices/" + indexName + ".json");
    nlohmann::json indexState;
    settings_file >> indexState;

    int dim = indexState["dimension"];
    std::string space = indexState["spaceType"];
    int ef_construction = indexState["efConstruction"];
    int M = indexState["M"];

    hnswlib::SpaceInterface<float>* metricSpace = (space == "IP")
        ? static_cast<hnswlib::SpaceInterface<float>*>(new hnswlib::InnerProductSpace(dim))
        : static_cast<hnswlib::SpaceInterface<float>*>(new hnswlib::L2Space(dim));

    auto *index = new hnswlib::HierarchicalNSW<float>(
        metricSpace,
        DEFAULT_INDEX_SIZE,
        M,
        ef_construction,
        42,
        true
    );
    std::string index_path = "indices/" + indexName + ".bin";
    index->loadIndex(index_path, metricSpace, 10000);

    indices[indexName] = index;
    indexSettings[indexName] = indexState;
}

int main() {
    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);

    CROW_ROUTE(app, "/health").methods(crow::HTTPMethod::GET)
    ([]() {
        return "OK";
    });

    CROW_ROUTE(app, "/create_index").methods(crow::HTTPMethod::POST)
    ([](const crow::request &req) {
        auto data = nlohmann::json::parse(req.body);
        IndexRequest indexRequest = data.get<IndexRequest>();

        {
            std::unique_lock<std::shared_mutex> indexLock(indexMutex);
            std::lock_guard<std::mutex> datastoreLock(dataStoreMutex);

            if (indices.find(indexRequest.indexName) != indices.end()) {
                return crow::response(400, "Index already exists");
            }

            hnswlib::SpaceInterface<float>* space = (indexRequest.spaceType == "IP")
                ? static_cast<hnswlib::SpaceInterface<float>*>(new hnswlib::InnerProductSpace(indexRequest.dimension))
                : static_cast<hnswlib::SpaceInterface<float>*>(new hnswlib::L2Space(indexRequest.dimension));

            auto *index = new hnswlib::HierarchicalNSW<float>(
                space,
                DEFAULT_INDEX_SIZE,
                indexRequest.M,
                indexRequest.efConstruction,
                42,
                true
            );

            indices[indexRequest.indexName] = index;
            indexSettings[indexRequest.indexName] = data;
            dataStores[indexRequest.indexName] = new DataStore();
        }
        return crow::response(200, "Index created");
    });

    CROW_ROUTE(app, "/load_index").methods(crow::HTTPMethod::POST)
    ([](const crow::request &req) {
        auto data = nlohmann::json::parse(req.body);
        std::string indexName = data["indexName"];
        {
            std::unique_lock<std::shared_mutex> indexLock(indexMutex);
            std::lock_guard<std::mutex> datastoreLock(dataStoreMutex);
        
            if (indices.find(indexName) != indices.end()) {
                return crow::response(400, "Index already exists");
            }

            read_index_from_disk(indexName);

            dataStores[indexName] = new DataStore();
            dataStores[indexName]->deserialize("indices/" + indexName + ".data");
        }

        return crow::response(200, "Index loaded");
    });

    CROW_ROUTE(app, "/save_index").methods(crow::HTTPMethod::POST)
    ([](const crow::request &req) {
        auto data = nlohmann::json::parse(req.body);
        std::string indexName = data["indexName"];

        {
            std::unique_lock<std::shared_mutex> indexLock(indexMutex);
            std::lock_guard<std::mutex> datastoreLock(dataStoreMutex);
            
            if (indices.find(indexName) == indices.end()) {
                return crow::response(404, "Index not found");
            }

            write_index_to_disk(indexName);
            dataStores[indexName]->serialize("indices/" + indexName + ".data");
        }
        return crow::response(200, "Index saved");
    });

    CROW_ROUTE(app, "/delete_index").methods(crow::HTTPMethod::POST)
    ([](const crow::request &req) {
        auto data = nlohmann::json::parse(req.body);
        std::string indexName = data["indexName"];

        {
            std::unique_lock<std::shared_mutex> indexLock(indexMutex);
            std::lock_guard<std::mutex> datastoreLock(dataStoreMutex);
            
            if (indices.find(indexName) == indices.end()) {
                return crow::response(404, "Index not found");
            }

            delete indices[indexName];
            indices.erase(indexName);
            indexSettings.erase(indexName);
            delete dataStores[indexName];
            dataStores.erase(indexName);
        }
        return crow::response(200, "Index deleted");
    });

    CROW_ROUTE(app, "/delete_index_from_disk").methods(crow::HTTPMethod::POST)
    ([](const crow::request &req) {
        auto data = nlohmann::json::parse(req.body);
        std::string indexName = data["indexName"];

        {
            std::unique_lock<std::shared_mutex> indexLock(indexMutex);
            std::lock_guard<std::mutex> datastoreLock(dataStoreMutex);
            
            if (indices.find(indexName) != indices.end()) {
                return crow::response(400, "Index is loaded. Please unload it first");
            }

            remove_index_from_disk(indexName);
        }
        return crow::response(200, "Index deleted from disk");
    });

    CROW_ROUTE(app, "/list_indices").methods(crow::HTTPMethod::GET)
    ([]() {
        nlohmann::json response;
        for (auto const& [indexName, _] : indices) {
            response.push_back(indexName);
        }

        return crow::response(response.dump());
    });

    CROW_ROUTE(app, "/add_documents").methods(crow::HTTPMethod::POST)
    ([](const crow::request &req) {
        auto data = nlohmann::json::parse(req.body);
        AddDocumentsRequest addReq = data.get<AddDocumentsRequest>();

        if (addReq.ids.size() != addReq.vectors.size()) {
            return crow::response(400, "Number of IDs does not match number of vectors");
        }

        if (addReq.metadatas.size() > 0 && addReq.metadatas.size() != addReq.ids.size()) {
            return crow::response(400, "Number of metadatas does not match number of IDs");
        }

        if (indices.find(addReq.indexName) == indices.end()) {
            return crow::response(404, "Index not found");
        }

        {
            std::unique_lock<std::shared_mutex> lock(indexMutex);
            auto *index = indices[addReq.indexName];

            int current_size = index->cur_element_count;
                
            if (current_size + addReq.ids.size() > index->max_elements_) {
                index->resizeIndex(current_size + current_size*INDEX_GROWTH_FACTOR + addReq.ids.size());
            }
        }
        
        {
            std::shared_lock<std::shared_mutex> lock(indexMutex);
            for (int i = 0; i < addReq.ids.size(); i++) {
                // std::vector<float> vec_data(addReq.vectors[i].begin(), addReq.vectors[i].end());
                std::vector<float>& vec_data = addReq.vectors[i];
                indices[addReq.indexName]->addPoint(vec_data.data(), addReq.ids[i], 0);
                if (addReq.metadatas.size()) {
                    dataStores[addReq.indexName]->set(addReq.ids[i], addReq.metadatas[i]);
                }
            }
        }
        

        return crow::response(200, "Documents added");
    });

    CROW_ROUTE(app, "/delete_documents").methods(crow::HTTPMethod::POST)
    ([](const crow::request &req) {
        auto data = nlohmann::json::parse(req.body);
        DeleteDocumentsRequest deleteReq = data.get<DeleteDocumentsRequest>();

        if (indices.find(deleteReq.indexName) == indices.end()) {
            return crow::response(404, "Index not found");
        }

        auto *index = indices[deleteReq.indexName];

        for (int id : deleteReq.ids) {
            index->markDelete(id);
            dataStores[deleteReq.indexName]->remove(id);
        }

        return crow::response(200, "Documents deleted");
    });

    CROW_ROUTE(app, "/search").methods(crow::HTTPMethod::POST)
    ([](const crow::request &req) {
        auto data = nlohmann::json::parse(req.body);
        SearchRequest searchReq = data.get<SearchRequest>();

        if (indices.find(searchReq.indexName) == indices.end()) {
            return crow::response(404, "Index not found");
        }

        auto *index = indices[searchReq.indexName];
        std::vector<float>& query_vec = searchReq.queryVector;
        index->setEf(searchReq.efSearch);


        std::priority_queue<std::pair<float, hnswlib::labeltype>> result;

        if (searchReq.filter.size() > 0) {
            std::shared_ptr<FilterASTNode> filters = parseFilters(searchReq.filter);
            std::unordered_set<int> filteredIds = dataStores[searchReq.indexName]->filter(filters);
            FilterIdsInSet filter(filteredIds);
            result = index->searchKnn(query_vec.data(), searchReq.k, &filter);
        } else {
            result = index->searchKnn(query_vec.data(), searchReq.k);
        }
         
        nlohmann::json response;
        std::vector<int> ids;
        std::vector<float> distances;
        while (!result.empty()) {
            ids.push_back(result.top().second);
            distances.push_back(result.top().first);
            result.pop();
        }

        std::reverse(ids.begin(), ids.end());
        std::reverse(distances.begin(), distances.end());

        response["hits"] = ids;
        response["distances"] = distances;

        return crow::response(response.dump());
    });

    std::cout << "Server started on port 8685!" << std::endl;
    std::cout << "Press Ctrl+C to quit" << std::endl;
    std::cout << "All other stdout is suppressed as an optimisation" << std::endl;

    // Start the server
    app.port(8685).multithreaded().run();
}
