#define DUCKDB_EXTENSION_MAIN

#include "gsheets_extension.hpp"
#include "gsheets_auth.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

#include <sstream>
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include <string>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <regex>
using namespace std;

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>

#include <json.hpp>

using json = nlohmann::json;

#include <fstream>

// Secrets
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"


namespace duckdb {




static std::string perform_https_request(const std::string& host, const std::string& path, const std::string& token) {
    std::string response;
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        throw duckdb::IOException("Failed to create SSL context");
    }

    BIO *bio = BIO_new_ssl_connect(ctx);
    SSL *ssl;
    BIO_get_ssl(bio, &ssl);
    SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);

    BIO_set_conn_hostname(bio, (host + ":443").c_str());

    if (BIO_do_connect(bio) <= 0) {
        BIO_free_all(bio);
        SSL_CTX_free(ctx);
        throw duckdb::IOException("Failed to connect");
    }

    std::string request = "GET " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + "\r\n";
    request += "Authorization: Bearer " + token + "\r\n";
    request += "Connection: close\r\n\r\n";

    if (BIO_write(bio, request.c_str(), request.length()) <= 0) {
        BIO_free_all(bio);
        SSL_CTX_free(ctx);
        throw duckdb::IOException("Failed to write request");
    }

    char buffer[1024];
    int len;
    while ((len = BIO_read(bio, buffer, sizeof(buffer))) > 0) {
        response.append(buffer, len);
    }

    BIO_free_all(bio);
    SSL_CTX_free(ctx);

    // Extract body from response
    size_t body_start = response.find("\r\n\r\n");
    if (body_start != std::string::npos) {
        return response.substr(body_start + 4);
    }

    return response;
}

static std::string fetch_sheet_data(const std::string& sheet_id, const std::string& token, const std::string& sheet_name) {
    std::string host = "sheets.googleapis.com";
    std::string path = "/v4/spreadsheets/" + sheet_id + "/values/" + sheet_name;
    
    return perform_https_request(host, path, token);
}

struct ReadSheetBindData : public TableFunctionData {
    string sheet_id;
    string token;
    bool finished;
    idx_t row_index;
    string response;
    bool header;
    string sheet_name;  // Add this line

    ReadSheetBindData(string sheet_id, string token, bool header, string sheet_name) 
        : sheet_id(sheet_id), token(token), finished(false), row_index(0), header(header), sheet_name(sheet_name) {
        response = fetch_sheet_data(sheet_id, token, sheet_name);  // Update this line
    }
};

#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

struct SheetData {
    std::string range;
    std::string majorDimension;
    std::vector<std::vector<std::string>> values;
};

SheetData parseJson(const std::string& json_str) {
    SheetData result;
    try {
        // Find the start of the JSON object
        size_t start = json_str.find('{');
        if (start == std::string::npos) {
            throw std::runtime_error("No JSON object found in the response");
        }

        // Find the end of the JSON object
        size_t end = json_str.rfind('}');
        if (end == std::string::npos) {
            throw std::runtime_error("No closing brace found in the JSON response");
        }

        // Extract the JSON object
        std::string clean_json = json_str.substr(start, end - start + 1);

        json j = json::parse(clean_json);

        if (j.contains("range") && j.contains("majorDimension") && j.contains("values")) {
            result.range = j["range"].get<std::string>();
            result.majorDimension = j["majorDimension"].get<std::string>();
            result.values = j["values"].get<std::vector<std::vector<std::string>>>();
        } else if (j.contains("error")) {
            string message = j["error"]["message"].get<std::string>();
            int code = j["error"]["code"].get<int>();
            throw std::runtime_error("Google Sheets API error: " + std::to_string(code) + " - " + message);
        } else {
            std::cerr << "JSON does not contain expected fields" << std::endl;
            std::cerr << "Raw JSON string: " << json_str << std::endl;
            throw;
        }
    } catch (const json::exception& e) {
        std::cerr << "JSON parsing error: " << e.what() << std::endl;
        std::cerr << "Raw JSON string: " << json_str << std::endl;
        throw;
    }

    return result;
}

static void ReadSheetFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = const_cast<ReadSheetBindData&>(data_p.bind_data->Cast<ReadSheetBindData>());

    if (bind_data.finished) {
        return;
    }

    SheetData sheet_data = parseJson(bind_data.response);

    idx_t row_count = 0;
    idx_t column_count = output.ColumnCount();

    // Adjust starting index based on whether we're using the header
    idx_t start_index = bind_data.header ? bind_data.row_index + 1 : bind_data.row_index;

    for (idx_t i = start_index; i < sheet_data.values.size() && row_count < STANDARD_VECTOR_SIZE; i++) {
        const auto& row = sheet_data.values[i];
        for (idx_t col = 0; col < column_count; col++) {
            if (col < row.size()) {
                output.SetValue(col, row_count, Value(row[col]));
            } else {
                output.SetValue(col, row_count, Value(nullptr));
            }
        }
        row_count++;
    }

    bind_data.row_index += row_count;
    bind_data.finished = (bind_data.row_index >= (sheet_data.values.size() - (bind_data.header ? 1 : 0)));

    output.SetCardinality(row_count);
}

static std::string extract_sheet_id(const std::string& input) {
    // Check if the input is already a sheet ID (no slashes)
    if (input.find('/') == std::string::npos) {
        return input;
    }

    // Regular expression to match the sheet ID in a Google Sheets URL
    if(input.find("docs.google.com/spreadsheets/d/") != std::string::npos) {
        std::regex sheet_id_regex("/d/([a-zA-Z0-9-_]+)");
        std::smatch match;

        if (std::regex_search(input, match, sheet_id_regex) && match.size() > 1) {
            return match.str(1);
        }
    }

    throw duckdb::InvalidInputException("Invalid Google Sheets URL or ID");
}

// Update the ReadSheetBind function
static unique_ptr<FunctionData> ReadSheetBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
    auto sheet_input = input.inputs[0].GetValue<string>();
    
    // Default values
    bool header = true;
    string sheet = "Sheet1";

    // Parse named parameters
    for (auto &kv : input.named_parameters) {
        if (kv.first == "header") {
            try {
                header = kv.second.GetValue<bool>();
            } catch (const std::exception& e) {
                throw InvalidInputException("Invalid value for 'header' parameter. Expected a boolean value.");
            }
        } else if (kv.first == "sheet") {
            sheet = kv.second.GetValue<string>();
        }
    }

    // Extract the sheet ID from the input (URL or ID)
    std::string sheet_id = extract_sheet_id(sheet_input);

    // Use the SecretManager to get the token
    auto &secret_manager = SecretManager::Get(context);
    auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
    auto secret_match = secret_manager.LookupSecret(transaction, "gsheet", "gsheet");
    
    if (!secret_match.HasMatch()) {
        throw InvalidInputException("No 'gsheet' secret found. Please create a secret with 'CREATE SECRET' first.");
    }

    auto &secret = secret_match.GetSecret();
    if (secret.GetType() != "gsheet") {
        throw InvalidInputException("Invalid secret type. Expected 'gsheet', got '%s'", secret.GetType());
    }

    const auto *kv_secret = dynamic_cast<const KeyValueSecret*>(&secret);
    if (!kv_secret) {
        throw InvalidInputException("Invalid secret format for 'gsheet' secret");
    }

    Value token_value;
    if (!kv_secret->TryGetValue("token", token_value)) {
        throw InvalidInputException("'token' not found in 'gsheet' secret");
    }

    std::string token = token_value.ToString();

    auto bind_data = make_uniq<ReadSheetBindData>(sheet_id, token, header, sheet);

    SheetData sheet_data = parseJson(bind_data->response);

    if (!sheet_data.values.empty()) {
        if (header) {
            for (const auto& column_name : sheet_data.values[0]) {
                names.push_back(column_name);
                return_types.push_back(LogicalType::VARCHAR);
            }
        } else {
            // If not using header, generate column names
            for (size_t i = 0; i < sheet_data.values[0].size(); i++) {
                names.push_back("column" + std::to_string(i + 1));
                return_types.push_back(LogicalType::VARCHAR);
            }
        }
    }

    return bind_data;
}

static void LoadInternal(DatabaseInstance &instance) {
    // Initialize OpenSSL
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    // Register read_gsheet table function
    TableFunction read_gsheet_function("read_gsheet", {LogicalType::VARCHAR}, ReadSheetFunction, ReadSheetBind);
    read_gsheet_function.named_parameters["header"] = LogicalType::BOOLEAN;
    read_gsheet_function.named_parameters["sheet"] = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(instance, read_gsheet_function);

    // Register Secret functions
	CreateGsheetSecretFunctions::Register(instance);

}

void GsheetsExtension::Load(DuckDB &db) {
	LoadInternal(*db.instance);
}
std::string GsheetsExtension::Name() {
	return "gsheets";
}

std::string GsheetsExtension::Version() const {
#ifdef EXT_VERSION_GSHEETS
	return EXT_VERSION_GSHEETS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_EXTENSION_API void gsheets_init(duckdb::DatabaseInstance &db) {
    duckdb::DuckDB db_wrapper(db);
    db_wrapper.LoadExtension<duckdb::GsheetsExtension>();
}

DUCKDB_EXTENSION_API const char *gsheets_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
