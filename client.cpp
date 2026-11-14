// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <netdb.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const char *TEMP_TDBG_FILE = "ttdb_temp.tdbg";
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;

// These two must be the same as in Phase 1

const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader 
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; 
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char* buf, size_t n)
{
    size_t totalReceived = 0;

    while (totalReceived < n)
    {
        ssize_t bytesReceived = recv(sock, buf + totalReceived, n - totalReceived, 0);

        if (bytesReceived < 0)
        {
            if (errno == EINTR)
                continue;

            return false;
        }

        if (bytesReceived == 0)
            return false;

        totalReceived += bytesReceived;
    }

    return true;
}

bool sendAll(int sock, const char* buf, size_t n)
{
    size_t totalSent = 0;

    while (totalSent < n)
    {
        ssize_t bytesSent = send(sock, buf + totalSent, n - totalSent, MSG_NOSIGNAL);

        if (bytesSent < 0)
        {
            if (errno == EINTR)
                continue;

            return false;
        }

        if (bytesSent == 0)
            return false;

        totalSent += bytesSent;
    }

    return true;
}

int connectToHost(const char* host, int port)
{
    struct addrinfo hints{};
    struct addrinfo* result = nullptr;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portString[10];
    snprintf(portString, sizeof(portString), "%d", port);

    int status = getaddrinfo(host, portString, &hints, &result);

    if (status != 0)
    {
        cout << "Host lookup failed!" << endl;
        return -1;
    }

    int clientSocket = -1;

    for (struct addrinfo* current = result; current != nullptr; current = current->ai_next)
    {
        clientSocket = socket(current->ai_family, current->ai_socktype, current->ai_protocol);

        if (clientSocket < 0)
            continue;

        if (connect(clientSocket, current->ai_addr, current->ai_addrlen) == 0)
            break;

        close(clientSocket);
        clientSocket = -1;
    }

    freeaddrinfo(result);

    if (clientSocket < 0)
        cout << "Connection to Load Balancer failed!" << endl;

    return clientSocket;
}

bool uploadSource(int sock, const char* sourcePath)
{
    FILE* file = fopen(sourcePath, "rb");

    if (file == nullptr)
    {
        cout << "Source file could not be opened!" << endl;
        return false;
    }

    unsigned char status = 0;
    uint32_t crc = 0xFFFFFFFF;

    crc = updateCRC32(crc, (char*)&status, 1);

    if (!sendAll(sock, (char*)&status, 1))
    {
        fclose(file);
        return false;
    }

    char buffer[IO_BUFFER_SIZE];

    while (true)
    {
        int bytesRead = fread(buffer, 1, IO_BUFFER_SIZE, file);

        if (bytesRead > 0)
        {
            crc = updateCRC32(crc, buffer, bytesRead);

            if (!sendAll(sock, buffer, bytesRead))
            {
                fclose(file);
                return false;
            }
        }

        if (bytesRead < IO_BUFFER_SIZE)
        {
            if (ferror(file))
            {
                fclose(file);
                return false;
            }

            break;
        }
    }

    fclose(file);
    crc = crc ^ 0xFFFFFFFF;

    if (!sendAll(sock, (char*)&crc, 4))
        return false;

    shutdown(sock, SHUT_WR);
    return true;
}

bool receiveResponse(int sock, const char* outTdbgPath, string& outError)
{
    TrailerChecker checker;
    ofstream file;
    unsigned char status = 0;
    bool statusReceived = false;
    char buffer[IO_BUFFER_SIZE];

    while (true)
    {
        int bytesReceived = recv(sock, buffer, IO_BUFFER_SIZE, 0);

        if (bytesReceived < 0)
        {
            if (errno == EINTR)
                continue;

            file.close();
            remove(outTdbgPath);
            return false;
        }

        if (bytesReceived == 0)
            break;

        for (int i = 0; i < bytesReceived; i++)
        {
            char bodyByte;
            bool hasBody;

            checker.addByte(buffer[i], bodyByte, hasBody);

            if (hasBody)
            {
                if (!statusReceived)
                {
                    status = (unsigned char)bodyByte;
                    statusReceived = true;

                    if (status == 0)
                    {
                        file.open(outTdbgPath, ios::binary);

                        if (!file.is_open())
                        {
                            remove(outTdbgPath);
                            return false;
                        }
                    }
                }

                else
                {
                    if (status == 0)
                        file.put(bodyByte);

                    else if (status == 1)
                        outError += bodyByte;
                }
            }
        }
    }

    if (!checker.verify() || !statusReceived)
    {
        file.close();
        remove(outTdbgPath);

        outError = "checksum mismatch, response corrupted";
        return false;
    }

    if (status == 1)
    {
        outError = "Server error: " + outError;
        return false;
    }

    if (status != 0)
    {
        file.close();
        remove(outTdbgPath);
        outError = "Invalid response status";
        return false;
    }

    file.close();

    if (!file)
    {
        remove(outTdbgPath);
        outError = "Could not write debugging file.";
        return false;
    }

    return true;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

bool readString(FILE* file, string& text)
{
    int32_t size;

    if (fread(&size, sizeof(int32_t), 1, file) != 1)
        return false;

    if (size < 0 || size > 1000000)
        return false;

    text.resize(size);

    if (fread(&text[0], sizeof(char), size, file) != (size_t)size)
    {
        text.clear();
        return false;
    }

    return true;
}

bool readVariable(FILE* file, Variable& variable)
{
    if (!readString(file, variable.name))
        return false;

    if (fread(&variable.value, sizeof(int32_t), 1, file) != 1)
        return false;

    return true;
}

bool readFrame(FILE* file, Frame& frame)
{
    if (!readString(file, frame.func_name))
        return false;

    if (fread(&frame.argc, sizeof(int32_t), 1, file) != 1)
        return false;

    if (frame.argc < 0 || frame.argc > MAX_VARS_PER_FRAME)
        return false;

    for (int i = 0; i < frame.argc; i++)
        if (!readVariable(file, frame.argv[i]))
            return false;

    if (fread(&frame.returnLine, sizeof(int32_t), 1, file) != 1)
        return false;

    if (fread(&frame.localCount, sizeof(int32_t), 1, file) != 1)
        return false;

    if (frame.localCount < 0 || frame.localCount > MAX_VARS_PER_FRAME)
        return false;

    for (int i = 0; i < frame.localCount; i++)
        if (!readVariable(file, frame.locals[i]))
            return false;

    return true;
}

bool readSnapshot(FILE* file, Snapshot& snapshot)
{
    if (fread(&snapshot.stackDepth, sizeof(int32_t), 1, file) != 1)
        return false;

    if (snapshot.stackDepth < 0 || snapshot.stackDepth > MAX_STACK_DEPTH)
        return false;

    for (int i = 0; i < snapshot.stackDepth; i++)
        if (!readFrame(file, snapshot.callStack[i]))
            return false;

    return true;
}

bool openTdbg(TdbgReader& reader, const char* fileName)
{
    FILE* file = fopen(fileName, "rb");

    if (file == nullptr)
        return false;

    // Read header fields separately (20 bytes total)
    if (fread(reader.header.magic, 1, 4, file) != 4 ||
        fread(&reader.header.version, sizeof(int32_t), 1, file) != 1 ||
        fread(&reader.header.stepCount, sizeof(int32_t), 1, file) != 1 ||
        fread(&reader.header.indexOffset, sizeof(int64_t), 1, file) != 1)
    {
        fclose(file);
        return false;
    }

    if (reader.header.magic[0] != 'T' ||
        reader.header.magic[1] != 'T' ||
        reader.header.magic[2] != 'D' ||
        reader.header.magic[3] != 'B' ||
        reader.header.version != 1 ||
        reader.header.stepCount < 0 ||
        reader.header.indexOffset < 20)
    {
        fclose(file);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return false;
    }

    int64_t fileSize = ftell(file);

    if (fileSize < 0 || reader.header.indexOffset > fileSize || reader.header.stepCount >
        (fileSize - reader.header.indexOffset) / sizeof(int64_t))
    {
        fclose(file);
        return false;
    }

    if (fseek(file, reader.header.indexOffset, SEEK_SET) != 0)
    {
        fclose(file);
        return false;
    }

    reader.index.clear();

    for (int64_t i = 0; i < reader.header.stepCount; i++)
    {
        int64_t offset;

        if (fread(&offset, sizeof(int64_t), 1, file) != 1)
        {
            reader.index.clear();
            fclose(file);
            return false;
        }

        if (offset < 20 || offset >= reader.header.indexOffset)
        {
            reader.index.clear();
            fclose(file);
            return false;
        }

        if (i > 0 && offset <= reader.index[i - 1])
        {
            reader.index.clear();
            fclose(file);
            return false;
        }

        reader.index.push_back(offset);
    }

    reader.file_index = dup(fileno(file));

    if (reader.file_index < 0)
    {
        fclose(file);
        return false;
    }

    fclose(file);
    return true;
}

void closeTdbg(TdbgReader& reader)
{
    if (reader.file_index >= 0)
    {
        close(reader.file_index);
        reader.file_index = -1;
    }

    reader.index.clear();
}

Snapshot* readSnapshotAt(TdbgReader& r, int32_t step)
{
    if (step < 0 || step >= r.header.stepCount || step >= (int32_t)r.index.size())
        return nullptr;

    int64_t offset = r.index[step];

    if (lseek(r.file_index, offset, SEEK_SET) < 0)
        return nullptr;

    FILE* file = fdopen(dup(r.file_index), "rb");

    if (file == nullptr)
        return nullptr;

    Snapshot* snapshot = new Snapshot;

    if (!readSnapshot(file, *snapshot))
    {
        delete snapshot;
        snapshot = nullptr;
    }

    fclose(file);
    return snapshot;
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession& s)
{
    s.active_flag = 0;
    s.reader.file_index = -1;
    s.currentStep = 0;
}

void closeSession(DebugSession& s)
{
    if (s.active_flag)
    {
        closeTdbg(s.reader);
        s.active_flag = 0;
    }
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession& s)
{
    Snapshot* snapshot = readSnapshotAt(s.reader, s.currentStep);

    if (snapshot == nullptr)
    {
        cout << "damaged .tdbg file" << endl;
        return false;
    }

    printSnapshot(s.currentStep, s.reader.header.stepCount, *snapshot);
    delete snapshot;
    return true;
}

bool moveBy(DebugSession& s, int32_t delta)
{
    int64_t newStep = s.currentStep + delta;

    if (newStep < 0 || newStep >= s.reader.header.stepCount)
    {
        cout << "No more snapshots in this direction." << endl;
        return false;
    }

    s.currentStep = newStep;
    return showCurrent(s);
}

bool requestDebugFile(const string& sourcePath, const char* outTdbgPath, string& error)
{
    int sock = connectToHost(LB_HOST.c_str(), LB_PORT);

    if (sock < 0)
    {
        error = "Could not connect to Load Balancer.";
        return false;
    }

    if (!uploadSource(sock, sourcePath.c_str()))
    {
        error = "Source file upload failed.";
        close(sock);
        return false;
    }

    bool success = receiveResponse(sock, outTdbgPath, error);
    close(sock);
    return success;
}

bool cmdStart(DebugSession& s, const string& sourcePath)
{
    string error;

    if (!requestDebugFile(sourcePath, TEMP_TDBG_FILE, error))
    {
        cout << error << endl;
        return false;
    }

    TdbgReader newReader;
    newReader.file_index = -1;

    if (!openTdbg(newReader, TEMP_TDBG_FILE))
    {
        cout << "server sent a damaged .tdbg file" << endl;
        remove(TEMP_TDBG_FILE);
        return false;
    }

    Snapshot* firstSnapshot = readSnapshotAt(newReader, 0);

    if (firstSnapshot == nullptr)
    {
        closeTdbg(newReader);
        remove(TEMP_TDBG_FILE);

        cout << "server sent a damaged .tdbg file" << endl;
        return false;
    }

    delete firstSnapshot;

    if (rename(TEMP_TDBG_FILE, TDBG_FILE) != 0)
    {
        closeTdbg(newReader);
        remove(TEMP_TDBG_FILE);

        cout << "Could not replace old session file." << endl;
        return false;
    }

    if (s.active_flag)
        closeTdbg(s.reader);

    s.reader = newReader;
    s.currentStep = 0;
    s.active_flag = 1;

    return showCurrent(s);
}

bool cmdNext(DebugSession& s)
{
    if (!s.active_flag)
    {
        cout << "No active debugging session." << endl;
        return false;
    }

    return moveBy(s, 1);
}

bool cmdPrv(DebugSession& s)
{
    if (!s.active_flag)
    {
        cout << "No active debugging session." << endl;
        return false;
    }

    return moveBy(s, -1);
}

bool cmdJmpNext(DebugSession& s, int32_t n)
{
    if (!s.active_flag)
    {
        cout << "No active debugging session." << endl;
        return false;
    }

    if (n <= 0)
    {
        cout << "Invalid jump count." << endl;
        return false;
    }

    return moveBy(s, n);
}

bool cmdJmpPrv(DebugSession& s, int32_t n)
{
    if (!s.active_flag)
    {
        cout << "No active debugging session." << endl;
        return false;
    }

    if (n <= 0)
    {
        cout << "Invalid jump count." << endl;
        return false;
    }

    return moveBy(s, -n);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s)
{
    cout << "\n========== Debug Snapshot ==========" << endl;

    cout << "Step: " << step + 1 << " / " << totalSteps << endl;
    cout << "Stack Depth: " << s.stackDepth << endl;

    for (int i = s.stackDepth - 1; i >= 0; i--)
    {
        const Frame& frame = s.callStack[i];

        cout << "\nFunction: " << frame.func_name << endl;
        cout << "Arguments:" << endl;

        for (int j = 0; j < frame.argc; j++)
            cout << "  " << frame.argv[j].name << " = " << frame.argv[j].value << endl;

        cout << "Return Line: " << frame.returnLine << endl;
        cout << "Local Variables:" << endl;

        for (int j = 0; j < frame.localCount; j++)
            cout << "  " << frame.locals[j].name << " = " << frame.locals[j].value << endl;
    }

    cout << "====================================" << endl;
}

void printHelp()
{
    cout << "\nAvailable Commands:\n" << endl;

    cout << "start <source_file>  - Start a new debugging session" << endl;
    cout << "next                 - Move to the next snapshot" << endl;
    cout << "prv                  - Move to the previous snapshot" << endl;
    cout << "jmp next <n>         - Jump n snapshots forward" << endl;
    cout << "jmp prv <n>          - Jump n snapshots backward" << endl;
    cout << "help                 - Display available commands" << endl;
    cout << "quit                 - Exit the debugger" << endl;
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession& s, const string& line, bool& quitRequested)
{
    vector<string> words;
    string word;
    stringstream input(line);

    while (input >> word)
        words.push_back(word);

    if (words.empty())
        return true;

    if (words[0] == "ttdb")
        words.erase(words.begin());

    if (words.empty())
        return true;

    if (words[0] == "start" && words.size() == 2)
        return cmdStart(s, words[1]);

    else if (words[0] == "next" && words.size() == 1)
        return cmdNext(s);

    else if (words[0] == "prv" && words.size() == 1)
        return cmdPrv(s);

    else if (words[0] == "jmp" && words.size() == 3)
    {
        char* end;
        long number = strtol(words[2].c_str(), &end, 10);

        if (*end != '\0' || number <= 0 || number > INT32_MAX)
        {
            cout << "unknown command" << endl;
            printHelp();
            return false;
        }

        if (words[1] == "next")
            return cmdJmpNext(s, (int32_t)number);

        else if (words[1] == "prv")
            return cmdJmpPrv(s, (int32_t)number);
    }

    else if (words[0] == "help" && words.size() == 1)
    {
        printHelp();
        return true;
    }

    else if ((words[0] == "quit" || words[0] == "exit") && words.size() == 1)
    {
        quitRequested = true;
        return true;
    }

    cout << "unknown command" << endl;
    printHelp();

    return false;
}

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        cout << "Usage: ./client <lb_ip> <lb_port>" << endl;
        return 1;
    }

    LB_HOST = argv[1];

    char* end;
    long port = strtol(argv[2], &end, 10);

    if (*end != '\0' || port < 1 || port > 65535)
    {
        cout << "Invalid port number!" << endl;
        return 1;
    }

    LB_PORT = (int)port;

    DebugSession session;
    initSession(session);
    cout << "Welcome to Time Travel Debugger!" << endl;
    printHelp();
    string line;
    bool quitRequested = false;

    while (!quitRequested)
    {
        cout << PROMPT;

        if (!getline(cin, line))
            break;

        runCommand(session, line, quitRequested);
    }

    closeSession(session);
    cout << "Debugger closed." << endl;

    return 0;
}