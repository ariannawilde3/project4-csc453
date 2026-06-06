# project4-csc453

## Questions 

1) Explain the difference between a symbolic link and a hard link. Why can a symbolic link cause an infinite loop during directory traversal, but a hard link to a directory cannot? (Hint: think about what the operating system allows you to create.) 

A symbolic link is a file that is separate from the item you are linking which stores a path to your linked file/directory. Because it has its own file, a symbolic link also has its own file system meta data, including its own inode. In contrast, a hard link is another directory entry that points directly to the same inode as the item being linked. This means that it has the same underlying file data, so altering the file through the hardlink alters the file directly. Given this structure, a symbolic link can point to any directory even if it is a parent or ancestor directory, and therefore during traversal it can send the program back to directories that it already visited and cause an infinate loop. However, for hardlinks the OS forbids hard links to point at directories, only to regular files. This prevents infinate loops from being created with hard links.  


2) Your cycle detection uses (st_dev, st_ino) pairs to identify directories. Why are both fields necessary? Give a concrete example of a scenario where checking only st_ino would incorrectly detect a cycle (or miss one).

Both of these fields are necessary because one refers to the inode number (st_ino) and the other refers to the device in which that inode belongs to (st_dev). Inode numbers are only guaranteed to be unique when they are within the same file system, therefore because you can have access to multiple filesystems you must use (st_dev, st_ino) pairs to make sure you are accessing the correct file, in the correct place. Otherwise, you may mistake two unrelated directories to be the same just because they have the same inode number. For example if I have two directories at these locations: 

- /users/schmitt/exams
- /devices/usb/answers

It is possible that they both have inode 123 even though they are at completely different loctions. Therefore if we only looked for inode 123 without st_dev, we may be looking at a file different than what we intended. Additonally, if we access the /exams directory and then look for the /answers directory, our program may think that there is a cycle because the /exams directory which has the same inode was already visited. 

3) When you run bfind / -xdev -name "*.conf", the traversal skips directories like /proc and /sys. Explain how your implementation detects that these directories are on a different filesystem. What field in struct stat changes when you cross a mount point, and why? 

Our implementation detects that these directories are on a different filesystem because it checks an entry's st_dev every time it visits a new entry by calling stat() or lstat(). As discussed in the previous question, the st_dev field helps identify what filesystem an entry belongs to. When the -xdev flag is enabled the program records the st_dev value of the original starting path. Therefore, when we check the st_dev for the new entry and see that it has changed, then we know we have crossed into a different filesystem and the program will skip it. Hence, directories like /proc and /sys which are backed by different file systems than the root "/" get skipped in this case.   

4) Linux presents a single unified directory tree (starting at /) even though it is composed of many different filesystems. Briefly explain the role of the VFS (Virtual File System) layer in making this possible. Why does the VFS need to exist — why can’t user programs just talk directly to each filesystem driver?

The VFS is a kernel layer which provides the common interface for all the file operations across different drivers. This makes it so that user programs can use standard POSIX system calls like open() and read() without knowing what filesystem a path is on. Further, the programs can't talk directly to the filesystem anyways because the drivers live inside kernel space. In order to navigate this, the VFS handles routing each operation to the correct filesystem driver, and abstracts all mounted filesystems into one unified directory tree starting at "/". This needs to exist because otherwise every program would need separate code to talk to each filesystem type directly and would result in complicated programs. 
