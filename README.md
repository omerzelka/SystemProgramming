CSE 344 - System Programming Projects 
  This repository contains advanced system-level projects developed for the CSE 344: 
System Programming course at Gebze Technical University. 
These assignments focus on the Unix programming environment, 
utilizing low-level C and POSIX APIs to interact directly with the operating system kernel.

-Development Environment
Operating System: Developed on macOS (utilizing XQuartz and Docker for Linux compatibility).
Language: C (Standard: POSIX / ISO C).
Build Tool: Makefile for automated compilation and memory leak testing (Valgrind).
Compiler: gcc / clang.


------------------------------------------------------------------------------------------------------
Assignment 1 Scenario:
  You are expected to write an “advanced” file search program for POSIX compatible operating
systems. Your program must be able to search for files satisfying the given criteria and print out the
results in the form of a nicely formatted tree.

-Usage Example:
  The search criteria can be any combination of the following (at least one of them must be
employed):
• -f : filename (case insensitive), supporting the following regular expression: +
• -b : file size (in bytes)
• -t : file type (d: directory, s: socket, b: block device, c: character device f: regular file, p:
pipe, l: symbolic link)
• -p : permissions, as 9 characters (e.g. ‘rwxr-xr--’)
• -l: number of links

->./myFind -w ./derin_test -f rapo+r 
------------------------------------------------------------------------------------------------------

------------------------------------------------------------------------------------------------------
Assignment 2 Scenario:
  You are expected to write an “advanced” file search program for POSIX compatible operating
systems. Your program must be able to search for files satisfying the given criteria and print out the
results in the form of a nicely formatted tree.

-Usage Example:
./procSearch -d <root_dir> -n <num_workers> -f <pattern> -s <min_size_bytes>
• -d <root_dir> Root directory to search (must exist)
• -n <num_workers> Number of worker processes to fork (between 2 and 8, inclusive)
• -f <pattern> Filename pattern; supports the + operator (see Section 1)
• -s <min_size> Optional. Match only files with size >= min_size bytes

->./procSearch -d . -n 4 -f repo+rt -s 1000
------------------------------------------------------------------------------------------------------

------------------------------------------------------------------------------------------------------
Assignment 3 Scenario:
  In this homework, you will design and implement a multi-process system that reads
words from a text file, distributes them across floors, transports their characters
between floors, and reconstructs them using concurrent sorting processes.
The goal of this assignment is not simply moving data from one place to another. Instead,
the main objective is:
How can multiple independent processes operate concurrently on shared data
structures while maintaining correctness, consistency, and synchronization?
This system includes:
• multiple processes working simultaneously,
• multiple active words at the same time,
• multiple character transfers occurring concurrently,
• multiple sorting processes operating on the same floor,
• shared data structures accessed concurrently,
• floor capacity constraints,
• two independent elevator subsystems,
• and a coordinated system-wide termination mechanism.
This assignment directly involves:
• process creation
• inter-process communication
• shared memory
• synchronization
• race conditions
• starvation, waiting, and deadlock risks
• controlled system termination
The design is also intentionally suitable for a thread-based extension in future work.

-Usage Example:

Number of Floors
-f <num_floors>
• Total number of floors in the system.
• Floors are assumed to be numbered from 0 to num_floors - 1.
Word-Carrier Processes per Floor
-w <word_carriers_per_floor>
• Number of word-carrier processes initially created for each floor.
Letter-Carrier Processes per Floor
-l <letter_carriers_per_floor>
• Number of letter-carrier processes initially created for each floor.
Sorting Processes per Floor
-s <sorting_processes_per_floor>
• Number of sorting processes created for each floor.
Floor Capacity (Active Words per Floor)
-c <max_words_per_floor>
• Maximum number of active words allowed on any floor.
• Used in admission control.
Delivery Elevator Capacity
-d <delivery_elevator_capacity>
• Maximum number of characters allowed to be inside the delivery elevator.
Reposition Elevator Capacity
-r <reposition_elevator_capacity>
• Maximum number of letter-carrier processes allowed to be inside the reposition
elevator.
Input File
-i <input_file>
• Path to the input .txt file.
Output File
-o <output_file>
• Path to the output .txt file to be generated at the end.

->./hw3 -f 5 -w 5 -l 5 -s 5 -c 5 -d 4 -r 3 -i input.txt -o output.txt 
------------------------------------------------------------------------------------------------------
