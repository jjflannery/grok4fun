#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <thread>
#include <mutex>
#define PCRE2_CODE_UNIT_WIDTH 8 // Use 8-bit characters (UTF-8 or ASCII)
#include <pcre2.h>

using namespace std;
namespace fs = filesystem;

struct FunctionCall {
    string filename;
    string fullCall;
    string argument;
    string tag;
    string identifier;
};

string escapeCSV(const string& str) {
    string result = str;
    if (result.find(',') != string::npos || result.find('"') != string::npos) {
        size_t pos = 0;
        while ((pos = result.find('"', pos)) != string::npos) {
            result.replace(pos, 1, "\"\"");
            pos += 2;
        }
        result = "\"" + result + "\"";
    }
    return result;
}

vector<FunctionCall> processFile(const string& filepath, pcre2_code* re, pcre2_match_data* match_data) {
    vector<FunctionCall> calls;
    ifstream file(filepath);
    string content;
    
    if (file.is_open()) {
        content.assign((istreambuf_iterator<char>(file)),
                       istreambuf_iterator<char>());
        file.close();
    } else {
        cerr << "Unable to open file: " << filepath << endl;
        return calls;
    }

    size_t pos = 0;
    const string marker = "myFunction";
    while ((pos = content.find(marker, pos)) != string::npos) {
        size_t start = pos;
        size_t max_len = min<size_t>(100, content.length() - start);
        string candidate = content.substr(start, max_len);

        // PCRE2 match
        const char* candidate_cstr = candidate.c_str();
        int rc = pcre2_match(re, reinterpret_cast<PCRE2_SPTR>(candidate_cstr), 
                            candidate.length(), 0, 0, match_data, nullptr);
        
        if (rc >= 0) {
            PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match_data);
            FunctionCall call;
            call.filename = fs::path(filepath).filename().string();
            
            // Full match
            call.fullCall = string(content.c_str() + start + ovector[0], ovector[1] - ovector[0]);
            
            // Tag (capture group 1)
            call.tag = string(candidate_cstr + ovector[2], ovector[3] - ovector[2]);
            
            // Identifier (capture group 2)
            call.identifier = string(candidate_cstr + ovector[4], ovector[5] - ovector[4]);
            
            call.argument = call.tag + " " + call.identifier;

            for (string* field : {&call.argument, &call.tag, &call.identifier}) {
                size_t startPos = field->find_first_not_of(" \t\n\r");
                size_t endPos = field->find_last_not_of(" \t\n\r");
                if (startPos != string::npos && endPos != string::npos) {
                    *field = field->substr(startPos, endPos - startPos + 1);
                } else if (startPos == string::npos) {
                    *field = "";
                }
            }

            calls.push_back(call);
        }
        pos += 1;
    }

    return calls;
}

void processFiles(const vector<string>& filepaths, vector<FunctionCall>& allCalls, 
                 size_t startIdx, size_t endIdx, pcre2_code* re, pcre2_match_data* match_data, mutex& mtx) {
    vector<FunctionCall> localCalls;
    for (size_t i = startIdx; i < endIdx && i < filepaths.size(); ++i) {
        if (fs::path(filepaths[i]).extension() == ".java") {
            vector<FunctionCall> fileCalls = processFile(filepaths[i], re, match_data);
            localCalls.insert(localCalls.end(), fileCalls.begin(), fileCalls.end());
        }
    }
    
    lock_guard<mutex> lock(mtx);
    allCalls.insert(allCalls.end(), localCalls.begin(), localCalls.end());
}

int main() {
    string directory;
    cout << "Enter directory path containing .java files: ";
    getline(cin, directory);

    // PCRE2 setup
    int errorcode;
    PCRE2_SIZE erroffset;
    pcre2_code* re = pcre2_compile(reinterpret_cast<PCRE2_SPTR>("myFunction\\s*\\(\"\\s*([a-zA-Z]+)\\s+([^\"]*)\"\\)\\s*;"),
                                  PCRE2_ZERO_TERMINATED, PCRE2_MULTILINE | PCRE2_DOTALL, 
                                  &errorcode, &erroffset, nullptr);
    if (!re) {
        PCRE2_UCHAR errorbuf[256];
        pcre2_get_error_message(errorcode, errorbuf, sizeof(errorbuf));
        cerr << "PCRE2 compilation failed at offset " << erroffset << ": " << errorbuf << endl;
        return 1;
    }

    // Create match data (shared per thread, but safe if not modified concurrently)
    pcre2_match_data* match_data = pcre2_match_data_create_from_pattern(re, nullptr);
    if (!match_data) {
        cerr << "Failed to create PCRE2 match data" << endl;
        pcre2_code_free(re);
        return 1;
    }

    vector<string> filepaths;
    try {
        for (const auto& entry : fs::directory_iterator(directory)) {
            filepaths.push_back(entry.path().string());
        }
    } catch (const fs::filesystem_error& e) {
        cerr << "Filesystem error: " << e.what() << endl;
        pcre2_match_data_free(match_data);
        pcre2_code_free(re);
        return 1;
    }

    const unsigned int numThreads = 16; // Core Ultra 7 155H
    vector<thread> threads;
    vector<FunctionCall> allCalls;
    mutex mtx;

    size_t filesPerThread = filepaths.size() / numThreads;
    for (unsigned int i = 0; i < numThreads; ++i) {
        size_t startIdx = i * filesPerThread;
        size_t endIdx = (i == numThreads - 1) ? filepaths.size() : (i + 1) * filesPerThread;
        threads.emplace_back(processFiles, ref(filepaths), ref(allCalls), 
                             startIdx, endIdx, re, match_data, ref(mtx));
    }

    for (auto& t : threads) {
        t.join();
    }

    ofstream csvFile("function_calls.csv");
    if (!csvFile.is_open()) {
        cerr << "Unable to create output CSV file" << endl;
        pcre2_match_data_free(match_data);
        pcre2_code_free(re);
        return 1;
    }

    csvFile << "Filename,Function Call,Argument,Tag,Identifier\n";
    for (const auto& call : allCalls) {
        csvFile << escapeCSV(call.filename) << ","
                << escapeCSV(call.fullCall) << ","
                << escapeCSV(call.argument) << ","
                << escapeCSV(call.tag) << ","
                << escapeCSV(call.identifier) << "\n";
    }

    csvFile.close();
    pcre2_match_data_free(match_data);
    pcre2_code_free(re);
    cout << "Processed " << allCalls.size() << " function calls with " << numThreads 
         << " threads. Output written to function_calls.csv" << endl;

    return 0;
}
