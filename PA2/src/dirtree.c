//--------------------------------------------------------------------------------------------------
// System Programming                         I/O Lab                                   Spring 2024
//
/// @file
/// @brief resursively traverse directory tree and list all entries
/// @author <yourname>
/// @studid <studentid>
//--------------------------------------------------------------------------------------------------

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>
#include <assert.h>
#include <grp.h>
#include <pwd.h>

#define MAX_DIR 64            ///< maximum number of supported directories

/// @brief output control flags
#define F_DIRONLY   0x1       ///< turn on direcetory only option
#define F_SUMMARY   0x2       ///< enable summary
#define F_VERBOSE   0x4       ///< turn on verbose mode

/// @brief struct holding the summary
struct summary {
  unsigned int dirs;          ///< number of directories encountered
  unsigned int files;         ///< number of files
  unsigned int links;         ///< number of links
  unsigned int fifos;         ///< number of pipes
  unsigned int socks;         ///< number of sockets

  unsigned long long size;    ///< total size (in bytes)
};


/// @brief abort the program with EXIT_FAILURE and an optional error message
///
/// @param msg optional error message or NULL
void panic(const char *msg)
{
  if (msg) fprintf(stderr, "%s\n", msg);
  exit(EXIT_FAILURE);
}


/// @brief read next directory entry from open directory 'dir'. Ignores '.' and '..' entries
///
/// @param dir open DIR* stream
/// @retval entry on success
/// @retval NULL on error or if there are no more entries
struct dirent *getNext(DIR *dir)
{
  struct dirent *next;
  int ignore;

  do {
    errno = 0;
    next = readdir(dir);
    if (errno != 0) perror(NULL);
    ignore = next && ((strcmp(next->d_name, ".") == 0) || (strcmp(next->d_name, "..") == 0));
  } while (next && ignore);

  return next;
}


/// @brief qsort comparator to sort directory entries. Sorted by name, directories first.
///
/// @param a pointer to first entry
/// @param b pointer to second entry
/// @retval -1 if a<b
/// @retval 0  if a==b
/// @retval 1  if a>b
static int dirent_compare(const void *a, const void *b)
{
  struct dirent *e1 = *(struct dirent **)a;
  struct dirent *e2 = *(struct dirent **)b;

  // if one of the entries is a directory, it comes first
  if (e1->d_type != e2->d_type) {
    if (e1->d_type == DT_DIR) return -1;
    if (e2->d_type == DT_DIR) return 1;
  }

  // otherwise sorty by name
  return strcmp(e1->d_name, e2->d_name);
}

struct dirent **readDirEntries(DIR *dir, int *cnt) {
  int capacity = 10;
  struct dirent **entries = malloc(capacity * sizeof(struct dirent *));
  // error handling
  if(!entries) {
    panic("Out of memory.\n");
    return NULL;
  }

  *cnt = 0;
  struct dirent *next;

  while((next = getNext(dir)) != NULL) {
    if( *cnt >= capacity ) {
      // dynamically increase entries 
      capacity *= 2;
      struct dirent **newEntries = realloc(entries, capacity * sizeof(struct dirent *));
      // error handling
      if(!newEntries) {
        panic("Out of memory.\n");
        return NULL;
      }
      entries = newEntries;
    }
    entries[(*cnt)++] = next;
  }

  return entries;
}

char* abbreviateFileName(unsigned int depth, const char *fileName) {  
  int indent = (depth+1) * 2;
  char* name = malloc(55);
  if(!name) {
    panic("Out of memory.\n");
    return NULL;
  }
  
  memset(name, 0, 55);
  memset(name, ' ', indent);
  size_t fileNameLength = strlen(fileName);
  
  if(fileNameLength+indent > 54) {
    strncpy((char*)(name+indent), fileName, 51-indent);
    strncpy((char*)(name+51), "...", 3);
  } else {
    strncpy((char*)(name+indent), fileName, fileNameLength);
  }

  return name;
}

/// @brief recursively process directory @a dn and print its tree
///
/// @param dn absolute or relative path string
/// @param depth depth in directory tree
/// @param stats pointer to statistics
/// @param flags output control flags (F_*)
///  struct dirent {  size 0x118
///    long d_ino;
///    off_t d_off;
///    unsigned short d_reclen;
///    char d_name[NAME_MAX+1];
///  }
void processDir(const char *dn, unsigned int depth, struct summary *stats, unsigned int flags)
{
  // open directory
  errno = 0;
  DIR *dd = opendir(dn);
  
  // failed to open directory
  if(dd == NULL && errno != 0) {
    for(int i=0; i<depth+1; i++) printf("  ");
    printf("ERROR: Permission denied\n");
    return;
  }

  // get all the directories & files
  int cnt;
  struct dirent **entries = readDirEntries(dd, &cnt);
  // qsort
  qsort(entries, cnt, sizeof(struct dirent *), dirent_compare);
  
  struct stat metadata;
  struct passwd *pwd;
  struct group *grp;
  int type;
  char type_c;
  char* abbreviateName;
  char permission[10] = { 0, };

  // print entries in directory
  for(int i=0; i<cnt; i++) {
    // need combined path to get stat
    int combinedPathSize = strlen(dn) + strlen(entries[i]->d_name) + 2;
    char *combinedPath = (char*) malloc(combinedPathSize);
    if(!combinedPath) {
      panic("Out of memory.\n");
      return;
    }
    snprintf(combinedPath, combinedPathSize, "%s/%s", dn, entries[i]->d_name);
    abbreviateName = abbreviateFileName(depth, entries[i]->d_name);

    if(lstat(combinedPath, &metadata) < 0) {
      // failed to read metadata

      // assume directories with no 'x' permission have no subdirectories (always file)
      stats->files++;
      if(flags & F_DIRONLY) continue;

      // prints 'permission denied' only in -v mode
      if(flags & F_VERBOSE) {
        printf("%-54s", abbreviateName);
        printf("  %s\n", "Permission denied");
      } else {
        for(int i=0; i<=depth; i++) printf("  ");
        printf("%s\n", entries[i]->d_name);
      }
      continue;
    } else {
      pwd = getpwuid(metadata.st_uid);
      grp = getgrgid(metadata.st_gid);
      type = metadata.st_mode & S_IFMT;

      // update statistics
      switch(type) {
        case S_IFDIR: // 디렉터리
          stats->dirs++;
          type_c = 'd';
          break;
        case S_IFREG: // 일반 파일
          stats->files++;
          type_c = ' ';
          break;
        case S_IFLNK: // (심볼릭) 링크 파일
          stats->links++;
          type_c = 'l';
          break;
        case S_IFIFO: // 파이프
          stats->fifos++;
          type_c = 'f';
          break;
        case S_IFSOCK: // 소켓 파일
          stats->socks++;
          type_c = 's';
          break;
        case S_IFCHR: // 문자 장치 특수 파일
          type_c = 'c';
          break;
        case S_IFBLK: // 블록 장치 특수 파일
          type_c = 'd';
          break;
        default: // 기타
          type_c = ' ';
      } 
      stats->size += metadata.st_size;

      // get permission
      permission[0] = (metadata.st_mode & S_IRUSR) ? 'r' : '-';
      permission[1] = (metadata.st_mode & S_IWUSR) ? 'w' : '-';
      permission[2] = (metadata.st_mode & S_IXUSR) ? 'x' : '-';
      permission[3] = (metadata.st_mode & S_IRGRP) ? 'r' : '-';
      permission[4] = (metadata.st_mode & S_IWGRP) ? 'w' : '-';
      permission[5] = (metadata.st_mode & S_IXGRP) ? 'x' : '-';
      permission[6] = (metadata.st_mode & S_IROTH) ? 'r' : '-';
      permission[7] = (metadata.st_mode & S_IWOTH) ? 'w' : '-';
      permission[8] = (metadata.st_mode & S_IXOTH) ? 'x' : '-';

      // directory only
      if((flags & F_DIRONLY) && (type != S_IFDIR)) continue;

      // print details or not
      if(flags & F_VERBOSE) {
        printf("%-54s", abbreviateName);
        printf("  %8s:%-8s", (pwd != NULL) ? pwd->pw_name : "Unknown", (grp != NULL) ? grp->gr_name : "Unknown");
        printf("  %10ld", metadata.st_size);
        printf(" %s  %c\n", permission, type_c);
      } else {
        for(int i=0; i<depth+1; i++) printf("  ");
        printf("%s\n", entries[i]->d_name);
      }

      // process further
      if(type == S_IFDIR) {
        processDir(combinedPath, depth+1, stats, flags);
      }
      free(combinedPath);
    }
    free(abbreviateName);   
  }

  free(entries);
  closedir(dd);
}


/// @brief print program syntax and an optional error message. Aborts the program with EXIT_FAILURE
///
/// @param argv0 command line argument 0 (executable)
/// @param error optional error (format) string (printf format) or NULL
/// @param ... parameter to the error format string
void syntax(const char *argv0, const char *error, ...)
{
  if (error) {
    va_list ap;

    va_start(ap, error);
    vfprintf(stderr, error, ap);
    va_end(ap);

    printf("\n\n");
  }

  assert(argv0 != NULL);

  fprintf(stderr, "Usage %s [-d] [-s] [-v] [-h] [path...]\n"
                  "Gather information about directory trees. If no path is given, the current directory\n"
                  "is analyzed.\n"
                  "\n"
                  "Options:\n"
                  " -d        print directories only\n"
                  " -s        print summary of directories (total number of files, total file size, etc)\n"
                  " -v        print detailed information for each file. Turns on tree view.\n"
                  " -h        print this help\n"
                  " path...   list of space-separated paths (max %d). Default is the current directory.\n",
                  basename(argv0), MAX_DIR);

  exit(EXIT_FAILURE);
}


/// @brief program entry point
int main(int argc, char *argv[])
{
  //
  // default directory is the current directory (".")
  //
  const char CURDIR[] = ".";
  const char *directories[MAX_DIR];
  int   ndir = 0;

  struct summary tstat;
  unsigned int flags = 0;

  //
  // parse arguments
  //
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      // format: "-<flag>"
      if      (!strcmp(argv[i], "-d")) flags |= F_DIRONLY;
      else if (!strcmp(argv[i], "-s")) flags |= F_SUMMARY;
      else if (!strcmp(argv[i], "-v")) flags |= F_VERBOSE;
      else if (!strcmp(argv[i], "-h")) syntax(argv[0], NULL);
      else syntax(argv[0], "Unrecognized option '%s'.", argv[i]);
    } else {
      // anything else is recognized as a directory
      if (ndir < MAX_DIR) {
        directories[ndir++] = argv[i];
      } else {
        printf("Warning: maximum number of directories exceeded, ignoring '%s'.\n", argv[i]);
      }
    }
  }

  // if no directory was specified, use the current directory
  if (ndir == 0) directories[ndir++] = CURDIR;

  // reset statistics (tstat)
  memset(&tstat, 0, sizeof(tstat));
  
  struct summary dstat;
  const char line[] = "----------------------------------------------------------------------------------------------------\n";
  const char *labels = (flags & F_VERBOSE) ? "%s%60s:%s%15s%10s %s\n" : "%s\n";
  const char *stats = (flags & F_VERBOSE) ? "%-68s   %14llu\n\n" : "%s\n\n";
  char *buf;

  // loop over all entries in 'directories'
  for(int i=0; i<ndir; i++) {
    memset(&dstat, 0, sizeof(dstat));

    // print (header and) dir name
    if(flags & F_SUMMARY) {
      printf(labels, "Name", "User", "Group", "Size", "Perms", "Type");
      printf(line);
    }
    printf("%s\n", directories[i]);

    // process directory
    processDir(directories[i], 0, &dstat, flags);

    // print summary
    if(flags & F_SUMMARY) {
      printf(line);
      if(flags & F_DIRONLY) {
        printf("%d director%s\n\n", dstat.dirs, (dstat.dirs==1) ? "y" : "ies");

        // update 
        tstat.dirs += dstat.dirs;
      } else {
        buf = malloc(69); // summary line
        if(!buf) {
          panic("Out of memory.\n");
          return 0;
        }
        memset(buf, 0, 69);

        snprintf(buf, 68, "%d file%s, %d director%s, %d link%s, %d pipe%s, and %d socket%s",
          dstat.files, (dstat.files==1) ? "" : "s",
          dstat.dirs, (dstat.dirs==1) ? "y" : "ies",
          dstat.links, (dstat.links==1) ? "" : "s",
          dstat.fifos, (dstat.fifos==1) ? "" : "s",
          dstat.socks, (dstat.socks==1) ? "" : "s"
        );
        printf(stats, buf, dstat.size);
        
        // update
        tstat.files += dstat.files;
        tstat.dirs += dstat.dirs;
        tstat.links += dstat.links;
        tstat.fifos += dstat.fifos;
        tstat.socks += dstat.socks;
        tstat.size += dstat.size;

        free(buf);
      }
    }
  }
  
  //
  // print grand total
  //
  if ((flags & F_SUMMARY) && (ndir > 1)) {
    if(flags & F_DIRONLY) {
      printf("Analyzed %d directories:\n"
           "  total # of directories:  %16d\n",
           ndir, tstat.dirs);
    } else {
      printf("Analyzed %d directories:\n"
           "  total # of files:        %16d\n"
           "  total # of directories:  %16d\n"
           "  total # of links:        %16d\n"
           "  total # of pipes:        %16d\n"
           "  total # of sockets:      %16d\n",
           ndir, tstat.files, tstat.dirs, tstat.links, tstat.fifos, tstat.socks);

      if (flags & F_VERBOSE) {
        printf("  total file size:         %16llu\n", tstat.size);
      }
    }
  }
  return EXIT_SUCCESS;
}
