#include <iostream>
#include <string>
#include <vector>
#include <cctype>
#include <set>
#include <algorithm>
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

// Helper: if a string literal, `quoted` identifier or comment starts at i,
// return the index just past it; otherwise return i unchanged
size_t skipLiteral(const std::string& query, size_t i) {
    char c = query[i];
    if (c == '\'' || c == '"' || c == '`') {
        // Skip to the closing quote, honouring backslash escapes in strings
        ++i;
        while (i < query.size() && query[i] != c) {
            if (query[i] == '\\' && c != '`') ++i;
            ++i;
        }
        return std::min(i + 1, query.size());
    }
    if (query.compare(i, 2, "//") == 0) {
        size_t newline = query.find('\n', i);
        return (newline == std::string::npos) ? query.size() : newline + 1;
    }
    if (query.compare(i, 2, "/*") == 0) {
        size_t close = query.find("*/", i + 2);
        return (close == std::string::npos) ? query.size() : close + 2;
    }
    return i;
}

// Function to detect statements that modify the graph. These must not be split:
// each branch would commit in its own transaction, in no fixed order.
// Errs on the safe side - a false positive only means the statement is not parallelised.
bool isWriteQuery(const std::string& query) {
    static const std::string writeKeywords[] = {"CREATE", "MERGE", "DELETE", "SET", "REMOVE", "FOREACH"};

    for (size_t i = 0; i < query.size();) {
        size_t next = skipLiteral(query, i);
        if (next != i) {
            i = next;
            continue;
        }
        for (const auto& keyword : writeKeywords) {
            if (matchKeyword(query, i, keyword)) return true;
        }
        ++i;
    }
    return false;
}

// One statement broken into UNION branches that can run in parallel
struct SplitStatement {
    std::vector<std::string> branches;
    bool distinct = false;  // true for UNION: duplicate rows must be removed after merging
};

// Function to split one statement into its top-level UNION / UNION ALL branches.
// UNION inside string literals, `quoted` identifiers, comments and nested
// (), {}, [] blocks (e.g. CALL { ... UNION ... } subqueries) is left alone.
// Write statements, and statements mixing UNION with UNION ALL (a Neo4j error),
// come back as a single branch so Neo4j runs them unchanged.
SplitStatement splitStatement(const std::string& query) {
    SplitStatement result;
    if (isWriteQuery(query)) {
        result.branches.push_back(trim(query));
        return result;
    }

    size_t start = 0;  // Start of the current branch
    int depth = 0;     // Nesting level of (), {}, []
    bool sawUnion = false;
    bool sawUnionAll = false;
    size_t i = 0;

    while (i < query.size()) {
        size_t next = skipLiteral(query, i);
        if (next != i) {
            i = next;
            continue;
        }

        char c = query[i];
        if (c == '(' || c == '{' || c == '[') {
            ++depth;
            ++i;
        } else if (c == ')' || c == '}' || c == ']') {
            --depth;
            ++i;
        } else if (depth == 0 && matchKeyword(query, i, "UNION")) {
            std::string branch = trim(query.substr(start, i - start));
            if (!branch.empty()) result.branches.push_back(branch);

            // Consume UNION and an optional ALL
            i += std::string("UNION").size();
            size_t afterUnion = query.find_first_not_of(" \t\r\n", i);
            if (afterUnion != std::string::npos && matchKeyword(query, afterUnion, "ALL")) {
                i = afterUnion + std::string("ALL").size();
                sawUnionAll = true;
            } else {
                sawUnion = true;
            }
            start = i;
        } else {
            ++i;
        }
    }

    std::string branch = trim(query.substr(start));  // Remaining part of the query
    if (!branch.empty()) result.branches.push_back(branch);

    if (sawUnion && sawUnionAll) {
        result.branches = {trim(query)};
        return result;
    }
    result.distinct = sawUnion;
    return result;
}

// Function to split every statement of a request; see splitStatement
std::vector<SplitStatement> splitQuery(const std::vector<std::string>& statements) {
    std::vector<SplitStatement> split;
    for (const auto& query : statements) {
        split.push_back(splitStatement(query));
    }
    return split;
}

// Function to merge the Neo4j responses of one statement's branches into a single
// result entry {"columns": [...], "data": [...]}, as Neo4j would return it for the
// unsplit statement. Rows keep branch order. For UNION (distinct) duplicate rows are
// dropped; rows are compared on their values and on node/relationship identity (the
// "meta" field), so two different nodes with equal properties are both kept.
// Problems are appended to errors and leave the statement's data empty.
nlohmann::json mergeBranchResults(const std::vector<nlohmann::json>& responses, bool distinct,
                                  nlohmann::json& errors) {
    nlohmann::json merged = {{"columns", nlohmann::json::array()}, {"data", nlohmann::json::array()}};
    std::set<std::string> seen;
    bool haveColumns = false;
    bool failed = false;

    for (size_t b = 0; b < responses.size(); ++b) {
        const auto& response = responses[b];
        const std::string branchName = "Branch " + std::to_string(b + 1);

        if (response.is_object() && response.contains("errors") && !response["errors"].empty()) {
            for (const auto& error : response["errors"]) errors.push_back(error);
            failed = true;
            continue;
        }
        if (!response.is_object() || !response.contains("results") || !response["results"].is_array() ||
            response["results"].empty()) {
            errors.push_back({{"code", "Client.RequestFailed"}, {"message", branchName + " returned no valid response"}});
            failed = true;
            continue;
        }

        const auto& result = response["results"][0];
        nlohmann::json columns = result.value("columns", nlohmann::json::array());
        if (!haveColumns) {
            merged["columns"] = columns;
            haveColumns = true;
        } else if (columns != merged["columns"]) {
            errors.push_back({{"code", "Client.ColumnMismatch"},
                              {"message", branchName + ": all sub queries in a UNION must return the same column names"}});
            failed = true;
            continue;
        }

        for (const auto& row : result.value("data", nlohmann::json::array())) {
            if (distinct) {
                nlohmann::json key = {row.value("row", nlohmann::json()), row.value("meta", nlohmann::json())};
                if (!seen.insert(key.dump()).second) continue;
            }
            merged["data"].push_back(row);
        }
    }

    if (failed) merged["data"] = nlohmann::json::array();
    return merged;
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
    // Read-only example: Alice matches both branches, so UNION must return her once
    const std::string query = R"({
        "statements" : [
            {
                "statement" : "MATCH (p:Person) WHERE p.name STARTS WITH 'A' RETURN p.name AS name UNION MATCH (p:Person) WHERE p.name ENDS WITH 'e' RETURN p.name AS name"
            },
            {
                "statement" : "MATCH (p:Person) RETURN count(p) AS people"
            }
        ]
    })";

    // Parse the JSON to extract the statements
    nlohmann::json json_query = nlohmann::json::parse(query);

    // Extract the "statement" values into the vector using the extractStatements function
    std::vector<std::string> statements = extractStatements(json_query);

    // Requests that write are sent unchanged, so Neo4j runs all statements
    // in one transaction and in order
    if (std::any_of(statements.begin(), statements.end(), isWriteQuery)) {
        std::cout << "Request modifies the graph, running it without splitting" << std::endl;
        std::cout << processSingleResponse(sendQuery(query)).dump(4) << std::endl;
        return 0;
    }

    // Read-only: split every statement into its UNION branches
    std::vector<SplitStatement> split = splitQuery(statements);

    // Start a separate thread for every branch of every statement
    std::vector<std::vector<std::future<std::string>>> futures(split.size());
    for (size_t s = 0; s < split.size(); ++s) {
        for (const auto& branch : split[s].branches) {
            futures[s].push_back(std::async(std::launch::async, sendQuery, buildRequest(branch)));
        }
    }

    // Reassemble one Neo4j-style response: one result per original statement, in order
    nlohmann::json combined = {{"results", nlohmann::json::array()}, {"errors", nlohmann::json::array()}};
    for (size_t s = 0; s < split.size(); ++s) {
        std::vector<nlohmann::json> responses;
        for (auto& future : futures[s]) {
            responses.push_back(processSingleResponse(future.get()));
        }
        combined["results"].push_back(mergeBranchResults(responses, split[s].distinct, combined["errors"]));
    }

    std::cout << "Combined Results: " << combined.dump(4) << std::endl;

    return 0;
}
#endif
