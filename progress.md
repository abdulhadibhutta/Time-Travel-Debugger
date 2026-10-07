#### &#x09;	    		Time Travel Debugger

##### \# Day 1 - Repository Setup:

###### &#x09;

###### &#x09;Date: 30 September 2026

&#x09;

&#x09;-> Created GitHub Repository

&#x09;-> Configured Git

&#x09;-> Cloned repository to my Laptop

&#x09;-> Read quickly to get the general idea without paying attention to details

&#x09;-> Designed and added GitHub repositories folders

&#x09;	. server.cpp:  contains all data structures implementation

&#x09;	. progress.md: contains information about each day of work done of project Phase 01





##### \# Day 2 - Architecture Analysis:



###### &#x09;Date: 01 October 2026



&#x09;-> Studied the project PDF with paying attention

&#x09;-> Studied the server.cpp template provided by the instructor

&#x09;-> Identified Project Stages

&#x09;	- Pass 0x0 Validation

&#x09;	- Pass 0x1 Resolve

&#x09;	- Pass 0x2 Execution

&#x09;	- Pass 0x3 Serialization

&#x09;

&#x09;-> Identified required custom data structures:

&#x09;	- Stack

&#x09;	- Timeline

&#x09;	- Snapshot

&#x09;	- Frame



&#x09;-> Implemented Stack data structure:

&#x09;	- Stack()

&#x09;	- push()

&#x09;	- pop()

&#x09;	- peek()

&#x09;	- isEmpty()

&#x09;	- depth()

&#x09;	- snapshot\_intro()



&#x09;-> Understood how stack will be used as the Call stack during execution phase





##### \# Day 3 - Timeline, Utility Functions and Validation



###### &#x09;Date: 02 October 2026



&#x09;-> Implemented Timeline data structure:

&#x09;	- Timeline()

&#x09;	- record()

&#x09;	- begin()

&#x09;	- getStepCount()



&#x09;-> Implemented Utility Functions:

&#x09;	- readSourceLine()

&#x09;	- firstWord()

&#x09;	- secondWord()



&#x09;-> Implemented Validation Pass (Pass 0x0):

&#x09;	- Matching func and func\_end validation

&#x09;	- Nested Function detection

&#x09;	- Duplicate function name detection

&#x09;	- Main function existence validation



&#x09;-> Used Vectors and its library to store function names

&#x09;-> Completed Pass 0x0 completely



##### \# Day 4 - Resolve Phase and Execution Preparation



###### &#x09;Date: 05 October 2026



&#x09;-> Implemented Resolve Pass (Pass 0x1):

&#x09;	- writeResolveRecord()

&#x09;	- readResolveRecord()

&#x09;	- resolveProgram()



&#x09;-> Implemented Tokenization system:

&#x09;	- tokenizeLine()



&#x09;-> Implementyed snapshot generation:

&#x09;	- buildSnapshot()



&#x09;-> Created my own source.bin file for testing

&#x09;-> Implemented variable helper functions:

&#x09;	- findVariable()

&#x09;	- getVariable()

&#x09;	- setVariable()



##### \# Day 5 - Execution Phase



###### &#x09;Date: 06 October 2026



&#x09;-> Implemented Execution Phase (Phase 0x2):

&#x09;	- executeProgram()

&#x09;	- Function call execution

&#x09;	- Function return handling

&#x09;	- Instruction execution



&#x09;-> Implemented function call stack using the custom Stack data structure

&#x09;-> Implemented stack frames:

&#x09;	- Created a new Frame for each function call

&#x09;	- Stored function arguments in the frame

&#x09;	- Stored local variables in the frame

&#x09;	- Stored return line information



&#x09;-> Implemented argument passing between functions

&#x09;-> Implemented argument write-back:

&#x09;	- Updated argument values are copied back to the caller after function return



&#x09;-> Implemented execution of C-- instructions:

&#x09;	- set

&#x09;	- add

&#x09;	- sub

&#x09;	- mul

&#x09;	- div



&#x09;-> Implemented variable value handling:

&#x09;	- getValue()

&#x09;	- setValue()



&#x09;-> Implemented Timeline snapshot generation during execution

&#x09;-> Tested the execution phase using a sample program with:

&#x09;	- main function

&#x09;	- foo function

&#x09;	- Function call with an argument

&#x09;	- Arithmetic operation

&#x09;	- Argument value write-back



&#x09;-> Verified that the updated argument value is correctly reflected in the caller



##### \# Day 6 - Serialization, Final Validation and Testing



###### &#x09;Date: 07 October 2026



&#x09;-> Completed Serialization Phase (Pass 0x3)

&#x09;-> Implemented TTDB header serialization:

&#x09;	- writeHeader()



&#x09;-> Implemented serialization helper functions:

&#x09;	- writeString()

&#x09;	- writeVariable()

&#x09;	- writeFrame()

&#x09;	- writeSnapshot()



&#x09;-> Implemented .tdbg file generation:

&#x09;	- writeTdbg()



&#x09;-> Implemented dense index trailer

&#x09;-> Updated the TTDB header with the final indexOffset

&#x09;-> Added undefined function call validation in Pass 0x0

&#x09;-> Added an additional unresolved function check in Pass 0x1

&#x09;-> Tested program validation:

&#x09;	- Undefined function call correctly rejected

&#x09;	- Duplicate function name correctly rejected

&#x09;	- Missing main function correctly rejected



&#x09;-> Tested final valid program:

&#x09;	- Function calls working correctly

&#x09;	- Argument passing working correctly

&#x09;	- Argument write-back working correctly

&#x09;	- Timeline generated 8 snapshots



&#x09;-> Removed temporary printTimeline() debugging function after final testing

&#x09;-> Generated and verified final session.tdbg file:

&#x09;	- File size: 440 bytes

&#x09;	- Magic: TTDB

&#x09;	- Version: 1

&#x09;	- Step count: 8

&#x09;	- Index offset: 376



&#x09;-> Completed Phase 01 server-side implementation and final testing

