#include <iostream>
#include <string>
#include <curl/curl.h>
#include <thread>
#include <future>
#include <vector>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <nlohmann/json.hpp>

// Function to Base64 encode the string
std::string base64_encode(const std::string& in) {
    BIO* bio;
    BIO* b64;
    BUF_MEM* bufferPtr;
    std::string out;

    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);
    BIO_write(bio, in.c_str(), in.length());
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &bufferPtr);
    out.assign(bufferPtr->data, bufferPtr->length - 1);  // Exclude the trailing null byte

    BIO_free_all(bio);
    return out;
}

// Function to handle the response data from the curl request
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Function to run a single Neo4j query and store the result using a promise
void run_query(const std::string& query, std::promise<std::string> &&result_promise, const std::string& auth_header) {
    std::string url = "http://localhost:7474/db/neo4j/tx/commit";
    std::string response;

    // Initialize curl
    CURL* curl = curl_easy_init();
    if (curl) {
        // Set the URL
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

        // Set the HTTP POST data
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, query.c_str());

        // Set the HTTP headers (Content-Type and Authorization)
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, auth_header.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Set the write callback function to capture the response
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        // Perform the HTTP request
        CURLcode res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
            result_promise.set_value("Error: " + std::string(curl_easy_strerror(res)));
        } else {
            // Set the result once the query completes
            result_promise.set_value(response);
        }

        // Clean up
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    } else {
        std::cerr << "Failed to initialize curl" << std::endl;
        result_promise.set_value("Error: Failed to initialize curl");
    }
}

int main() {
    // Neo4j connection details
    std::string username = "neo4j";
    std::string password = "testpassword";
    std::string auth_string = username + ":" + password;
    std::string base64_auth = base64_encode(auth_string);
    std::string auth_header = "Authorization: Basic " + base64_auth;

    // Split the main query into smaller subqueries based on UNION
    std::vector<std::string> queries = {
        R"({"statements" : [{"statement" : "CREATE (n:Person {name: 'Alice'}) RETURN n AS person"}]})",
        R"({"statements" : [{"statement" : "CREATE (m:Person {name: 'Bob'}) RETURN m AS person"}]})"
    };

    // Vector to store the futures of each query result
    std::vector<std::future<std::string>> futures;

    // Start a thread for each query
    for (const auto& query : queries) {
        std::promise<std::string> result_promise;
        futures.push_back(result_promise.get_future());

        // Run the query asynchronously in a new thread
        std::thread(run_query, query, std::move(result_promise), auth_header).detach();
    }

    // Wait for all threads to finish and combine the results
    nlohmann::json final_result;
    final_result["columns"] = {"person"};
    final_result["data"] = nlohmann::json::array();

    for (auto& future : futures) {
        std::string result = future.get();  // Get result from each thread

        // Parse the JSON response
        try {
            nlohmann::json response_json = nlohmann::json::parse(result);
            for (const auto& data : response_json["results"]) {
                for (const auto& row : data["data"]) {
                    final_result["data"].push_back(row);
                }
            }
        } catch (const nlohmann::json::parse_error& e) {
            std::cerr << "JSON parsing error: " << e.what() << std::endl;
        }
    }

    // Output the final result as a JSON string
    std::cout << "Combined Results: " << final_result.dump(4) << std::endl;

    return 0;
}
