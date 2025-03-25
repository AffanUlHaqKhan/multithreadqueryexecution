#include <iostream>
#include <string>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

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

int main() {
    // Neo4j connection details (updated URL)
    std::string url = "http://localhost:7474/db/neo4j/tx/commit";
    
    // Properly formatted Cypher query (escaped JSON)
    std::string query = R"({
        "statements" : [
          {
            "statement" : "CREATE (n:Person {name: 'Alice'}) RETURN n AS person UNION CREATE (m:Person {name: 'Bob'}) RETURN m AS person"
          }
        ]
      })";
    
    // Username and password for authentication
    std::string username = "neo4j";
    std::string password = "testpassword";
    
    // Combine username and password
    std::string auth_string = username + ":" + password;

    // Base64 encode the username and password
    std::string base64_auth = base64_encode(auth_string);

    // Prepare the Authorization header
    std::string auth_header = "Authorization: Basic " + base64_auth;
    
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
        } else {
            std::cout << "Response: " << response << std::endl;
        }

        // Clean up
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    } else {
        std::cerr << "Failed to initialize curl" << std::endl;
    }

    return 0;
}
