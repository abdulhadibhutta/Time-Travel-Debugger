// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    {
        top = nullptr;
        count = 0;
    }

    void push(const T &val)
    {
        if (count >= MAX_STACK_DEPTH)
            return;

        Node* newNode = new Node;
        newNode->data = val;
        newNode->next = top;
        top = newNode;
        count++;
    }

    T pop()
    {
        if (isEmpty())
            return T();

        Node* temp = top;
        T value = temp->data;
        top = top->next;
        delete temp;
        count--;
        return value;
    }

    T &peek()
    {
        return top->data;
    }
    bool isEmpty()
    {
        return top == nullptr;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        Node* curr = top;
        int32_t index = 0;

        while (curr != nullptr and index < maxLen)
        {
            out[index] = curr->data;
            curr = curr->next;
            index++;
        }

        return index;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        stepCount = 0;
    }

    void record(Snapshot *s)
    {
        TimelineNode* newNode = new TimelineNode;

        newNode->data = s;
        newNode->next = nullptr;
        newNode->prev = tail;

        if (head == nullptr)
        {
            head = newNode;
            tail = newNode;
        }

        else
        {
            tail->next = newNode;
            tail = newNode;
        }

        stepCount++;
    }

    TimelineNode *begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
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
    string argSourceNames[MAX_VARS_PER_FRAME];
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

void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    while (getline(in, out))
    {
        if (!out.empty() and out.back() == '\r')
            out.pop_back();

        if (out.length() > 0)
            return true;
    }

    return false;
}

string firstWord(const string &line)
{
    string word = "";
    int i = 0;

    while (i < line.length() and line[i] != ' ')
    {
        word = word + line[i];
        i++;
    }

    return word;
}

string secondWord(const string &line)
{
    string word = "";
    int i = 0;

    while (i < line.length() and line[i] != ' ')
        i++;

    while (i < line.length() and line[i] == ' ')
        i++;

    while (i < line.length() and line[i] != ' ')
    {
        word = word + line[i];
        i++;
    }

    return word;
}

bool validateProgram(const char* sourcePath)
{
    ifstream in(sourcePath);

    if (!in.is_open())
        return false;

    Stack<string> funcStack;
    vector<string> functionNames;
    vector<string> calledFunctions;

    string line;

    while (readSourceLine(in, line))
    {
        string keyword = firstWord(line);

        if (keyword == "func")
        {
            if (!funcStack.isEmpty())
                return false;

            string funcName = secondWord(line);

            if (funcName == "")
                return false;

            for (int i = 0; i < functionNames.size(); i++)
                if (functionNames[i] == funcName)
                    return false;

            functionNames.push_back(funcName);
            funcStack.push(funcName);
        }

        else if (keyword == "func_end")
        {
            if (funcStack.isEmpty())
                return false;

            funcStack.pop();
        }

        else if (keyword == "call")
        {
            string functionName = secondWord(line);

            if (functionName == "")
                return false;

            calledFunctions.push_back(functionName);
        }
    }

    if (!funcStack.isEmpty())
        return false;

    bool mainFound = false;

    for (int i = 0; i < functionNames.size(); i++)
    {
        if (functionNames[i] == "main")
        {
            mainFound = true;
            break;
        }
    }

    if (!mainFound)
        return false;

    for (int i = 0; i < calledFunctions.size(); i++)
    {
        bool functionFound = false;

        for (int j = 0; j < functionNames.size(); j++)
        {
            if (calledFunctions[i] == functionNames[j])
            {
                functionFound = true;
                break;
            }
        }

        if (!functionFound)
            return false;
    }

    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    int64_t startPos = ftell(f);
    int32_t size = text.length();

    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&size, sizeof(int32_t), 1, f);
    fwrite(text.c_str(), sizeof(char), size, f);

    return startPos;
}

int64_t readResolveRecord(FILE *f, string &outText)
{
    int64_t offsetField;
    int32_t size;

    fread(&offsetField, sizeof(int64_t), 1, f);
    fread(&size, sizeof(int32_t), 1, f);
    char* buffer = new char[size + 1];
    fread(buffer, sizeof(char), size, f);

    buffer[size] = '\0';
    outText = buffer;
    delete[] buffer;

    return offsetField;
}

int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream in(sourcePath);

    if (!in.is_open())
        return -1;

    FILE* out = fopen(resolveBinPath, "wb+");

    if (out == nullptr)
        return -1;

    string line;
    int64_t mainOffset = -1;

    while (readSourceLine(in, line))
    {
        string keyword = firstWord(line);

        if (keyword == "func")
        {
            string funcName = secondWord(line);
            int64_t recordPos = writeResolveRecord(out, -1, line);

            funcArray[funcCount].funcName = funcName;
            funcArray[funcCount].byteOffsetInResolveBin = recordPos;
            funcCount++;

            if (funcName == "main")
                mainOffset = recordPos;
        }

        else if (keyword == "call")
        {
            string targetFunc = secondWord(line);
            int64_t recordPos = writeResolveRecord(out, -1, line);

            patches[patchCount].byteOffsetOfOffsetField = recordPos;
            patches[patchCount].targetFuncName = targetFunc;
            patchCount++;
        }

        else
            writeResolveRecord(out, -1, line);
    }

    for (int i = 0; i < patchCount; i++)
    {
        int64_t targetOffset = -1;

        for (int j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName == patches[i].targetFuncName)
            {
                targetOffset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }

        if (targetOffset == -1)
            return -1;

        fseek(out, patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&targetOffset, sizeof(int64_t), 1, out);
    }

    fclose(out);
    in.close();

    return mainOffset;
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};

struct Token
{
    TokenType type;
    string text;
};

int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    vector<string> words;
    string current = "";

    for (int i = 0; i < line.length(); i++)
    {
        if (line[i] == ' ')
        {
            if (!current.empty())
            {
                words.push_back(current);
                current = "";
            }
        }

        else
            current = current + line[i];
    }

    if (!current.empty())
        words.push_back(current);

    int count = 0;

    for (int i = 0; i < words.size() and count < maxTokens; i++)
    {
        tokens[count].text = words[i];

        if (i == 0)
            tokens[count].type = KEYWORD;

        else if (i == 1)
            tokens[count].type = IDENTIFIER;

        else
            tokens[count].type = PARAM;

        count++;
    }

    return count;
}

Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    Snapshot* snapshot = new Snapshot;
    snapshot->stackDepth = callStack.snapshot_into(snapshot->callStack, MAX_STACK_DEPTH);
    return snapshot;
}

int findVariable(Frame& frame, const string& name)
{
    for (int i = 0; i < frame.localCount; i++)
        if (frame.locals[i].name == name)
            return i;

    return -1;
}

int getVariable(Frame& frame, const string& name)
{
    int index = findVariable(frame, name);

    if (index == -1)
        return 0;

    return frame.locals[index].value;
}

void setVariable(Frame& frame, const string& name, int value)
{
    int index = findVariable(frame, name);

    if (index != -1)
    {
        frame.locals[index].value = value;
        return;
    }

    if (frame.localCount < MAX_VARS_PER_FRAME)
    {
        frame.locals[frame.localCount].name = name;
        frame.locals[frame.localCount].value = value;
        frame.localCount++;
    }
}

bool isNumber(const string& text)
{
    if (text.empty())
        return false;

    int start = 0;

    if (text[0] == '-')
    {
        if (text.length() == 1)
            return false;

        start = 1;
    }

    for (int i = start; i < text.length(); i++)
        if (text[i] < '0' or text[i] > '9')
            return false;

    return true;
}

int getValue(Frame& frame, const string& name)
{
    int localIndex = findVariable(frame, name);

    if (localIndex != -1)
        return frame.locals[localIndex].value;

    for (int i = 0; i < frame.argc; i++)
        if (frame.argv[i].name == name)
            return frame.argv[i].value;

    if (isNumber(name))
        return stoi(name);

    return 0;
}

void setValue(Frame& frame, const string& name, int value)
{
    for (int i = 0; i < frame.argc; i++)
    {
        if (frame.argv[i].name == name)
        {
            frame.argv[i].value = value;
            return;
        }
    }

    setVariable(frame, name, value);
}

void copyArgumentsBack(Frame& callee, Frame& caller)
{
    for (int i = 0; i < callee.argc; i++)
    {
        if (callee.argSourceNames[i] != "")
        {
            setValue(caller,
                callee.argSourceNames[i],
                callee.argv[i].value);
        }
    }
}

void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    FILE* file = fopen(resolveBinPath, "rb");

    if (file == nullptr)
        return;

    Stack<Frame> callStack;
    fseek(file, mainOffset, SEEK_SET);

    string mainLine;
    int64_t mainRecordOffset = readResolveRecord(file, mainLine);

    Token mainTokens[MAX_TOKENS];
    int32_t mainTokenCount = tokenizeLine(mainLine, mainTokens, MAX_TOKENS);

    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.returnLine = -1;
    mainFrame.localCount = 0;

    for (int i = 2; i < mainTokenCount && mainFrame.argc < MAX_VARS_PER_FRAME; i++)
    {
        mainFrame.argv[mainFrame.argc].name = mainTokens[i].text;
        mainFrame.argv[mainFrame.argc].value = 0;
        mainFrame.argSourceNames[mainFrame.argc] = "";
        mainFrame.argc++;
    }

    callStack.push(mainFrame);
    int64_t currentOffset = mainOffset;

    while (!callStack.isEmpty())
    {
        fseek(file, currentOffset, SEEK_SET);

        string line;
        int64_t storedOffset = readResolveRecord(file, line);

        if (feof(file))
            break;

        int64_t nextOffset = ftell(file);

        Token tokens[MAX_TOKENS];
        int32_t tokenCount = tokenizeLine(line, tokens, MAX_TOKENS);

        if (tokenCount == 0)
        {
            currentOffset = nextOffset;
            continue;
        }

        string keyword = tokens[0].text;

        if (keyword == "func")
            currentOffset = nextOffset;

        else if (keyword == "set")
        {
            if (tokenCount >= 3)
            {
                Frame& currentFrame = callStack.peek();
                int value = getValue(currentFrame, tokens[2].text);
                setValue(currentFrame, tokens[1].text, value);
            }

            currentOffset = nextOffset;
        }

        else if (keyword == "add")
        {
            if (tokenCount >= 3)
            {
                Frame& currentFrame = callStack.peek();
                int left = getValue(currentFrame, tokens[1].text);
                int right = getValue(currentFrame, tokens[2].text);
                setValue(currentFrame, tokens[1].text, left + right);
            }

            currentOffset = nextOffset;
        }

        else if (keyword == "sub")
        {
            if (tokenCount >= 3)
            {
                Frame& currentFrame = callStack.peek();
                int left = getValue(currentFrame, tokens[1].text);
                int right = getValue(currentFrame, tokens[2].text);
                setValue(currentFrame, tokens[1].text, left - right);
            }

            currentOffset = nextOffset;
        }

        else if (keyword == "mul")
        {
            if (tokenCount >= 3)
            {
                Frame& currentFrame = callStack.peek();
                int left = getValue(currentFrame, tokens[1].text);
                int right = getValue(currentFrame, tokens[2].text);
                setValue(currentFrame, tokens[1].text, left * right);
            }

            currentOffset = nextOffset;
        }

        else if (keyword == "div")
        {
            if (tokenCount >= 3)
            {
                Frame& currentFrame = callStack.peek();
                int left = getValue(currentFrame, tokens[1].text);
                int right = getValue(currentFrame, tokens[2].text);

                if (right != 0)
                    setValue(currentFrame, tokens[1].text, left / right);
            }

            currentOffset = nextOffset;
        }

        else if (keyword == "call")
        {
            int64_t targetOffset = storedOffset;
            fseek(file, targetOffset, SEEK_SET);

            string targetLine;
            readResolveRecord(file, targetLine);

            Token targetTokens[MAX_TOKENS];
            int32_t targetTokenCount = tokenizeLine(targetLine, targetTokens, MAX_TOKENS);

            Frame caller = callStack.peek();

            Frame newFrame;
            newFrame.func_name = targetTokenCount >= 2 ? targetTokens[1].text : "";

            newFrame.argc = 0;
            newFrame.returnLine = (int32_t)nextOffset;
            newFrame.localCount = 0;

            int argumentIndex = 2;

            while (argumentIndex < tokenCount && newFrame.argc < MAX_VARS_PER_FRAME)
            {
                if (newFrame.argc + 2 >= targetTokenCount)
                    break;

                string formalName = targetTokens[newFrame.argc + 2].text;
                string actualName = tokens[argumentIndex].text;
                newFrame.argv[newFrame.argc].name = formalName;
                newFrame.argv[newFrame.argc].value = getValue(caller, actualName);
                newFrame.argSourceNames[newFrame.argc] = actualName;
                newFrame.argc++;
                argumentIndex++;
            }

            callStack.push(newFrame);
            currentOffset = targetOffset;
        }

        else if (keyword == "func_end")
        {
            Frame finishedFrame = callStack.pop();

            if (callStack.isEmpty())
            {
                Snapshot* snapshot = buildSnapshot(callStack);
                timeline.record(snapshot);
                break;
            }

            Frame& caller = callStack.peek();
            copyArgumentsBack(finishedFrame, caller);
            currentOffset = (int64_t)finishedFrame.returnLine;
        }

        Snapshot* snapshot = buildSnapshot(callStack);
        timeline.record(snapshot);
    }

    fclose(file);
}

void writeString(FILE* f, const string& text)
{
    int32_t size = text.length();

    fwrite(&size, sizeof(int32_t), 1, f);
    fwrite(text.c_str(), sizeof(char), size, f);
}

void writeVariable(FILE* f, const Variable& variable)
{
    writeString(f, variable.name);
    fwrite(&variable.value, sizeof(int32_t), 1, f);
}

void writeFrame(FILE* f, const Frame& frame)
{
    writeString(f, frame.func_name);

    fwrite(&frame.argc, sizeof(int32_t), 1, f);

    for (int i = 0; i < frame.argc; i++)
    {
        writeVariable(f, frame.argv[i]);
    }

    fwrite(&frame.returnLine, sizeof(int32_t), 1, f);

    fwrite(&frame.localCount, sizeof(int32_t), 1, f);

    for (int i = 0; i < frame.localCount; i++)
    {
        writeVariable(f, frame.locals[i]);
    }
}

void writeSnapshot(FILE* f, const Snapshot& snapshot)
{
    fwrite(&snapshot.stackDepth, sizeof(int32_t), 1, f);

    for (int i = 0; i < snapshot.stackDepth; i++)
    {
        writeFrame(f, snapshot.callStack[i]);
    }
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    FILE* file = fopen(tdbgPath, "wb+");

    if (file == nullptr)
        return;

    TTDBHeader header;

    header.magic[0] = 'T';
    header.magic[1] = 'T';
    header.magic[2] = 'D';
    header.magic[3] = 'B';

    header.version = 1;
    header.stepCount = timeline.getStepCount();
    header.indexOffset = 0;

    writeHeader(file, header);
    vector<int64_t> index;
    TimelineNode* current = timeline.begin();

    while (current != nullptr)
    {
        int64_t snapshotOffset = ftell(file);
        index.push_back(snapshotOffset);
        writeSnapshot(file, *current->data);
        current = current->next;
    }

    header.indexOffset = ftell(file);

    for (int i = 0; i < index.size(); i++)
        fwrite(&index[i], sizeof(int64_t), 1, file);

    fseek(file, 0, SEEK_SET);
    writeHeader(file, header);
    fclose(file);
}

int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        cout << "Validation Failed" << endl;
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}