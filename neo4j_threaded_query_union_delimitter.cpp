#include <iostream>
#include <string>
#include <vector>
#include <cctype>
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

// Helper: true if c can be part of an unquoted Cypher identifier or parameter
bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

// Helper: case-insensitive match of a whole keyword (e.g. "UNION") at pos.
// Rejects matches that are part of a longer word or a property access like n.union
bool matchKeyword(const std::string& s, size_t pos, const std::string& keyword) {
    if (pos + keyword.size() > s.size()) return false;
    if (pos > 0 && (isIdentChar(s[pos - 1]) || s[pos - 1] == '.')) return false;
    for (size_t i = 0; i < keyword.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(s[pos + i])) != keyword[i]) return false;
    }
    size_t end = pos + keyword.size();
    return end == s.size() || !isIdentChar(s[end]);
}

// Helper: strip leading and trailing whitespace
std::string trim(const std::string& s) {
    const char* whitespace = " \t\r\n";
    size_t begin = s.find_first_not_of(whitespace);
    if (begin == std::string::npos) return "";
    size_t end = s.find_last_not_of(whitespace);
    return s.substr(begin, end - begin + 1);
}

// Function to split each statement into its top-level UNION / UNION ALL branches.
// UNION inside string literals, `quoted` identifiers, comments and nested
// (), {}, [] blocks (e.g. CALL { ... UNION ... } subqueries) is left alone.
std::vector<std::string> splitQuery(const std::vector<std::string>& statements) {
    std::vector<std::string> subqueries;

    for (const auto& query : statements) {
        size_t start = 0;  // Start of the current branch
        int depth = 0;     // Nesting level of (), {}, []
        size_t i = 0;

        while (i < query.size()) {
            char c = query[i];

            if (c == '\'' || c == '"' || c == '`') {
                // Skip to the closing quote, honouring backslash escapes in strings
                char quote = c;
                ++i;
                while (i < query.size() && query[i] != quote) {
                    if (query[i] == '\\' && quote != '`') ++i;
                    ++i;
                }
                ++i;
            } else if (query.compare(i, 2, "//") == 0) {
                size_t newline = query.find('\n', i);
                i = (newline == std::string::npos) ? query.size() : newline + 1;
            } else if (query.compare(i, 2, "/*") == 0) {
                size_t close = query.find("*/", i + 2);
                i = (close == std::string::npos) ? query.size() : close + 2;
            } else if (c == '(' || c == '{' || c == '[') {
                ++depth;
                ++i;
            } else if (c == ')' || c == '}' || c == ']') {
                --depth;
                ++i;
            } else if (depth == 0 && matchKeyword(query, i, "UNION")) {
                std::string branch = trim(query.substr(start, i - start));
                if (!branch.empty()) subqueries.push_back(branch);

                // Consume UNION and an optional ALL
                i += std::string("UNION").size();
                size_t next = query.find_first_not_of(" \t\r\n", i);
                if (next != std::string::npos && matchKeyword(query, next, "ALL")) {
                    i = next + std::string("ALL").size();
                }
                start = i;
            } else {
                ++i;
            }
        }

        std::string branch = trim(query.substr(start));  // Remaining part of the query
        if (!branch.empty()) subqueries.push_back(branch);
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

// Function to wrap a single subquery in a Neo4j HTTP request body.
// Built with nlohmann::json so quotes/backslashes in the subquery are escaped
std::string buildRequest(const std::string& subquery) {
    nlohmann::json request;
    request["statements"] = nlohmann::json::array({{{"statement", subquery}}});
    return request.dump();
}

// main is left out when the file is compiled into the unit tests
#ifndef UNIT_TEST
int main() {
    const std::string query = R"({
        "statements" : [
            {
                "statement" : "CREATE (n:Person {name: 'Alice'}) RETURN n AS person UNION CREATE (m:Person {name: 'Bob'}) RETURN m AS person UNION CREATE (p:Person {name: 'Charlie'}) RETURN p AS person"
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
        futures.push_back(std::async(std::launch::async, sendQuery, buildRequest(subquery)));
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
#endif
