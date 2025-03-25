#include <iostream>
#include <string>
#include <vector>
#include <curl/curl.h>
#include <thread>
#include <future>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

// Helper function to handle response data from the curl request
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

// Function to Base64 encode the string
std::string base64_encode(const std::string& input) {
    BIO* bio = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    BIO_push(bio, mem);
    BIO_write(bio, input.c_str(), input.size());
    BIO_flush(bio);
    
    BUF_MEM* bufferPtr;
    BIO_get_mem_ptr(bio, &bufferPtr);
    
    std::string result(bufferPtr->data, bufferPtr->length - 1);  // Exclude the trailing null byte
    BIO_free_all(bio);
    return result;
}

// Function to send a query to Neo4j
std::string sendQuery(const std::string& query) {
    const std::string url = "http://localhost:7474/db/neo4j/tx/commit";
    const std::string username = "neo4j";
    const std::string password = "testpassword";
    
    std::string auth_header = "Authorization: Basic " + base64_encode(username + ":" + password);
    std::string response;

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Failed to initialize curl" << std::endl;
        return "";
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, query.c_str());
    
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "Request failed: " << curl_easy_strerror(res) << std::endl;
    }

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    
    return response;
}

// Function to process a single response
nlohmann::json processSingleResponse(const std::string& result) {
    nlohmann::json response_json;
    try {
        response_json = nlohmann::json::parse(result);
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "Error parsing JSON: " << e.what() << std::endl;
    }
    return response_json;
}

// Function to split a query by UNION and INTERSECT
std::vector<std::string> splitQuery(const std::vector<std::string>& statements) {
    std::vector<std::string> subqueries;
    const std::string delimiters[] = {"UNION", "INTERSECT"};

    for (const auto& query : statements) {
        size_t startPos = 0;
        size_t endPos = query.size();
        std::string innerQuery = query;

        // Split the query using UNION or INTERSECT as delimiters
        while (true) {
            size_t delimiterPos = std::string::npos;
            size_t minPos = std::string::npos;
            
            for (const auto& delimiter : delimiters) {
                size_t pos = innerQuery.find(delimiter, startPos);
                if (pos != std::string::npos && (minPos == std::string::npos || pos < minPos)) {
                    minPos = pos;
                    delimiterPos = pos;
                }
            }
            
            if (delimiterPos == std::string::npos) break;
            
            subqueries.push_back(innerQuery.substr(startPos, delimiterPos - startPos));
            startPos = delimiterPos + strlen(delimiters[0].c_str());
        }

        subqueries.push_back(innerQuery.substr(startPos));  // Remaining part of the query
    }

    return subqueries;
}

// Function to extract "statement" values into a vector
std::vector<std::string> extractStatements(const nlohmann::json& json_query) {
    std::vector<std::string> statements;
    
    // Extract the "statement" values into the vector
    for (const auto& statement_obj : json_query["statements"]) {
        statements.push_back(statement_obj["statement"]);
    }
    
    return statements;
}

int main() {
    const std::string query = R"({
        "statements" : [
            {
                "statement" : "CREATE (n:Person {name: 'Alice'}) RETURN n AS person UNION CREATE (m:Person {name: 'Bob'}) RETURN m AS person INTERSECT CREATE (p:Person {name: 'Charlie'}) RETURN p AS person"
            },
            {
                "statement" : "MATCH (p:Person) RETURN p"
            }
        ]
    })";

    // Parse the JSON to extract the statements
    nlohmann::json json_query = nlohmann::json::parse(query);
    
    // Extract the "statement" values into the vector using the extractStatements function
    std::vector<std::string> statements = extractStatements(json_query);

    // Split query into subqueries using the splitQuery function
    std::vector<std::string> subqueries = splitQuery(statements);

    // Create futures to handle queries in parallel
    std::vector<std::future<std::string>> futures;

    // Start a separate thread for each query
    for (const auto& subquery : subqueries) {
        std::string fullQuery = R"({"statements" : [{"statement" : ")" + subquery + R"("} ]})";
        futures.push_back(std::async(std::launch::async, sendQuery, fullQuery));
    }

    // Process and handle the results for each statement separately
    for (size_t i = 0; i < futures.size(); ++i) {
        std::string result = futures[i].get();  // Get the result from each thread
        nlohmann::json response_json = processSingleResponse(result);

        // Output the result for each statement separately
        std::cout << "Results for Statement " << (i + 1) << ": " << std::endl;
        std::cout << response_json.dump(4) << std::endl << std::endl;
    }

    return 0;
}
