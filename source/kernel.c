/*
 * Kernel
 */

#include "types.h"
#include "kernel.h"
#include "hw86.h"
#include "ulib/ulib.h"
#include "syscall.h"
#include "fs.h"

uchar serial_status;  /* Serial port status */
uint serial_debug = 0; /* Debug info through serial port */

uchar a20_enabled = 0; /* A20 line enabled */

uint screen_width  = 80; /* Screen size (text mode) */
uint screen_height = 50;

/*
 * Disk related
 */
uchar  system_disk; /* System disk */
struct DISKINFO disk_info[MAX_DISK];  /* Disk info */

/*
 * Extern program call
 */
typedef uint extern_main(uint argc, uchar* argv[]);
#define EXTERN_PROGRAM_MEMLOC 0xD000

/*
 * Heap related
 */
#define HEAP_MAX_BLOCK   0x0080U
#define HEAP_MEM_SIZE    0x2000U
#define HEAP_BLOCK_SIZE  (HEAP_MEM_SIZE / HEAP_MAX_BLOCK)

static uchar HEAPADDR[HEAP_MEM_SIZE]; /* Allocate heap memory */

struct HEAPBLOCK {
  uint   used;
  void*  ptr;
} heap[HEAP_MAX_BLOCK];

/*
 * Init heap: all blocks are unused
 */
static void heap_init()
{
  uint i;
  for(i=0; i<HEAP_MAX_BLOCK; i++) {
    heap[i].used = 0;
    heap[i].ptr = 0;
  }
}

/*
 * Allocate memory in heap
 */
static void* heap_alloc(uint size)
{
  uint n_alloc = 0;
  uint n_found = 0;
  uint i, j;

  if(size == 0) {
    return 0;
  }

  /* Get number of blocks to allocate */
  n_alloc = size / HEAP_BLOCK_SIZE +
    ((size % HEAP_BLOCK_SIZE) ? 1 : 0);

  /* Find a continuous set of n_alloc free blocks */
  for(i=0; i<HEAP_MAX_BLOCK; i++) {
    if(heap[i].used) {
      n_found = 0;
    } else {
      n_found++;
      if(n_found >= n_alloc) {
        uint bi = (i+1-n_alloc)*HEAP_BLOCK_SIZE;
        void* addr = &HEAPADDR[bi];
        for(j=i+1-n_alloc; j<=i; j++) {
          heap[j].ptr = addr;
          heap[j].used = 1;
        }

        return addr;
      }
    }
  }

  /* Error: not found */
  debugstr("Mem alloc: BAD ALLOC (%d bytes)\n\r", size);
  return 0;
}

/*
 * Free memory in heap
 */
static heap_free(void* ptr)
{
  uint i;
  if(ptr != 0) {
    for(i=0; i<HEAP_MAX_BLOCK; i++) {
      if(heap[i].ptr == ptr && heap[i].used) {
        heap[i].used = 0;
        heap[i].ptr = 0;
      }
    }
  }

  return;
}

#define LMEM_START 0x00010000
#define LMEM_LIMIT 0x00110000
#define LMEM_BLOCK_SIZE 0x10
#define LMEM_MAX_BLOCK 64
struct LMEMBLOCK {
  lptr     start;
  uint32_t size;
} lmem[LMEM_MAX_BLOCK];

/*
 * Init far memory: all blocks are unused
 */
static void lmem_init()
{
  memset(&lmem, 0, sizeof(lmem));
}

/*
 * Allocate far memory
 */
static lptr lmem_alloc(uint32_t size)
{
  lptr start = 0;
  uint i;

  /* If size is 0 or all blocks are used, return */
  if(size==0 || lmem[LMEM_MAX_BLOCK-1].size!=0) {
    return 0;
  }

  /* Align blocks to 16 bytes */
  size += size % (uint32_t)LMEM_BLOCK_SIZE;

  /* Find a continuous big enough free space */
  for(i=0; i<LMEM_MAX_BLOCK; i++) {
    /* If this block is allocated */
    if(lmem[i].start) {
      /* If there are no more blocks, set not found and break */
      if(i == LMEM_MAX_BLOCK-1) {
        start = 0;
        break;
      }
      /* A possible start is at the end of this block */
      start = lmem[i].start + lmem[i].size;
      /* If there is enough space, break */
      if(lmem[i+1].start==0 || lmem[i+1].start>start+size) {
        i++; /* The right place for the new block is after current block */
        break;
      }
    } else {
      /* Next blocks are free. If it's the first, set start address */
      if(start == 0) {
        start = (lptr)LMEM_START;
      }
      /* Set not found if there isn't enough space */
      if((lptr)LMEM_LIMIT-start < size) {
        start = 0;
      }
      break;
    }
  }

  /* Found if start != 0 */
  if(start != 0) {
    /* Allocate block at i */
    memcpy(&lmem[i+1], &lmem[i],
      (LMEM_MAX_BLOCK-i-1)*sizeof(struct LMEMBLOCK));

    lmem[i].start = start;
    lmem[i].size = size;
    return start;
  }

  /* Error: not found */
  debugstr("LMem alloc: BAD ALLOC (%U bytes)\n\r", size);
  return 0;
}

/*
 * Free far memory
 */
static void lmem_free(lptr ptr)
{
  uint i = 0;
  if(ptr != 0) {
    while(i<LMEM_MAX_BLOCK) {
      /* Find block */
      if(lmem[i].start == ptr) {
        /* Free block */
        lmem[i].start = 0;
        lmem[i].size = 0;

        /* Keep list ordered and contiguous */
        memcpy(&lmem[i], &lmem[i+1],
          (LMEM_MAX_BLOCK-i-1)*sizeof(struct LMEMBLOCK));
        lmem[LMEM_MAX_BLOCK-1].start = 0;
        lmem[LMEM_MAX_BLOCK-1].size = 0;
      } else {
        i++;
      }
    }
  }

  return;
}

/*
 * BCD to int
 */
static uint BCD_to_int(uchar BCD)
{
    uint h = (BCD >> 4) * 10;
    return h + (BCD & 0xF);
}

/*
 * Handle system calls
 * Usually:
 * -Unpack parameters
 * -Redirect to the right kernel function
 */
uint kernel_service(uint service, void* param)
{
  switch(service) {

    case SYSCALL_IO_GET_SCREEN_SIZE: {
      struct TSYSCALL_POSITION* ps = param;
      *(ps->px) = screen_width;
      *(ps->py) = screen_height;
      return 0;
    }

    case SYSCALL_IO_CLEAR_SCREEN:
      io_clear_screen();
      return 0;

    case SYSCALL_IO_OUT_CHAR:
      io_out_char((uchar)*param);
      return 0;

    case SYSCALL_IO_OUT_CHAR_ATTR: {
      struct TSYSCALL_CHARATTR* ca = param;
      io_out_char_attr(ca->x, ca->y, ca->c, ca->attr);
      return 0;
    }

    case SYSCALL_IO_SET_CURSOR_POS: {
      struct TSYSCALL_POSITION* ps = param;
      io_set_cursor_pos(ps->x, ps->y);
      return 0;
    }

    case SYSCALL_IO_GET_CURSOR_POS: {
      struct TSYSCALL_POSITION* ps = param;
      io_get_cursor_pos(ps->px, ps->py);
      return 0;
    }

    case SYSCALL_IO_SET_SHOW_CURSOR: {
      uint mode = (uint)*param;
      if(mode == HIDE_CURSOR) {
        io_hide_cursor();
      } else {
        io_show_cursor();
      }
      return 0;
    }

    case SYSCALL_IO_IN_KEY: {
      uint mode = (uint)*param;
      uint c;
      do {
        c = io_in_key();
      } while(c==0 && mode==WAIT_KEY);
      return c;
    }

    case SYSCALL_IO_OUT_CHAR_SERIAL:
      io_out_char_serial((uchar)*param);
      return 0;

    case SYSCALL_IO_IN_CHAR_SERIAL:
      return (uint)io_in_char_serial();

    case SYSCALL_IO_OUT_CHAR_DEBUG:
      if(serial_debug) {
        io_out_char_serial((uchar)*param);
      }
      return 0;

    case SYSCALL_MEM_ALLOCATE:
      return heap_alloc((uint)*param);

    case SYSCALL_MEM_FREE:
      heap_free(param);
      return 0;

    case SYSCALL_LMEM_ALLOCATE: {
      struct TSYSCALL_LMEM* lm = param;
      lm->dst = lmem_alloc(lm->n);
      return 0;
    }
    case SYSCALL_LMEM_FREE: {
      struct TSYSCALL_LMEM* lm = param;
      lmem_free(lm->dst);
      return 0;
    }
    case SYSCALL_LMEM_GET: {
      struct TSYSCALL_LMEM* lm = param;
      return lmem_getbyte(lm->dst);
    }
    case SYSCALL_LMEM_SET: {
      struct TSYSCALL_LMEM* lm = param;
      lmem_setbyte(lm->dst, (uchar)lm->n);
      return 0;
    }

    case SYSCALL_FS_GET_INFO: {
      struct TSYSCALL_FSINFO* fi = param;
      return fs_get_info(fi->disk_index, fi->info);
    }

    case SYSCALL_FS_GET_ENTRY: {
      struct TSYSCALL_FSENTRY* fi = param;
      struct SFS_ENTRY entry;
      uint result = fs_get_entry(&entry, fi->path, fi->parent, fi->disk);
      strcpy_s(fi->entry->name, entry.name, sizeof(fi->entry->name));
      fi->entry->flags = entry.flags;
      fi->entry->size = entry.size;
      return result;
    }

    case SYSCALL_FS_READ_FILE: {
      struct TSYSCALL_FSRWFILE* fi = param;
      return fs_read_file(fi->buff, fi->path, fi->offset, fi->count);
    }

    case SYSCALL_FS_WRITE_FILE: {
      struct TSYSCALL_FSRWFILE* fi = param;
      return fs_write_file(fi->buff, fi->path, fi->offset, fi->count, fi->flags);
    }

    case SYSCALL_FS_MOVE: {
      struct TSYSCALL_FSSRCDST* fi = param;
      return fs_move(fi->src, fi->dst);
    }

    case SYSCALL_FS_COPY: {
      struct TSYSCALL_FSSRCDST* fi = param;
      return fs_copy(fi->src, fi->dst);
    }

    case SYSCALL_FS_DELETE: {
      struct TSYSCALL_FSINFO* fi = param;
      return fs_delete((uchar*)param);
    }

    case SYSCALL_FS_CREATE_DIRECTORY:
      return fs_create_directory((uchar*)param);

    case SYSCALL_FS_LIST: {
      struct TSYSCALL_FSLIST* fi = param;
      struct SFS_ENTRY entry;
      uint result = fs_list(&entry, fi->path, fi->n);
      strcpy_s(fi->entry->name, entry.name, sizeof(fi->entry->name));
      fi->entry->flags = entry.flags;
      fi->entry->size = entry.size;
      return result;
    }

    case SYSCALL_FS_FORMAT:
      return fs_format((uint)*param);

    case SYSCALL_CLK_GET_TIME: {
      struct TIME* t = param;
      uchar BCDtime[3];
      uchar BCDdate[3];
      get_time(BCDtime, BCDdate);
      t->hour   = BCD_to_int(BCDtime[0]);
      t->minute = BCD_to_int(BCDtime[1]);
      t->second = BCD_to_int(BCDtime[2]);
      t->year   = BCD_to_int(BCDdate[0]) + 2000;
      t->month  = BCD_to_int(BCDdate[1]);
      t->day    = BCD_to_int(BCDdate[2]);
      return 0;
    }
  }

  return 0;
}

/*
 * The main kernel function
 */
#define CLI_MAX_ARG 4
void kernel()
{
  uint result;
  uint i;
  uint n;

  /* Some initialization is still needed */
  io_set_text_mode();
  io_show_cursor();
  io_clear_screen();

  /* Setup disk identifiers */
  disk_info[0].id = 0x00; /* Floppy disk 0 */
  strcpy_s(disk_info[0].name, "fd0", sizeof(disk_info[0].name));

  disk_info[1].id = 0x01; /* Floppy disk 1 */
  strcpy_s(disk_info[1].name, "fd1", sizeof(disk_info[1].name));

  disk_info[2].id = 0x80;   /* Hard disk 0 */
  strcpy_s(disk_info[2].name, "hd0", sizeof(disk_info[2].name));

  disk_info[3].id = 0x81;   /* Hard disk 1 */
  strcpy_s(disk_info[3].name, "hd1", sizeof(disk_info[3].name));

  /* Initialize hardware related disks info */
  for(i=0; i<MAX_DISK; i++) {
    n = index_to_disk(i);

    /* Try to get info */
    result = get_disk_info(n, &disk_info[i].sectors,
      &disk_info[i].sides, &disk_info[i].cylinders);

    /* Use retrieved data if success */
    if(result == 0) {
      disk_info[i].size = ((uint32_t)disk_info[i].sectors *
        (uint32_t)disk_info[i].sides * (uint32_t)disk_info[i].cylinders) /
        ((uint32_t)1048576 / (uint32_t)BLOCK_SIZE);

      debugstr("DISK (%x : size=%U MB sect_per_track=%d, sides=%d, cylinders=%d)\n\r",
        n, disk_info[i].size, disk_info[i].sectors, disk_info[i].sides,
        disk_info[i].cylinders);

    } else {
      /* Failed. Do not use this disk */
      disk_info[i].sectors = 0;
      disk_info[i].sides = 0;
      disk_info[i].cylinders = 0;
      disk_info[i].size = 0;
    }
  }

  /* Init heap */
  heap_init();

  /* Init far memory */
  lmem_init();

  /* Init FS info */
  fs_init_info();

  putstr("Starting...\n\r");
  debugstr("Starting...\n\r");

  /* Integrated CLI */
  while(1) {
    uint   argc;
    uchar* argv[CLI_MAX_ARG];
    uchar  str[72];
    uchar* tok = str;
    uchar* nexttok = tok;

    memset(str, 0, sizeof(str));
    memset(argv, 0, sizeof(argv));

    /* Prompt and wait command */
    putstr("> ");
    getstr(str, sizeof(str));
    debugstr("> %s\n\r", str);


    /* Tokenize */
    argc = 0;
    while(*tok && *nexttok && argc < CLI_MAX_ARG) {
      tok = strtok(tok, &nexttok, ' ');
      if(*tok) {
        argv[argc++] = tok;
      }
      tok = nexttok;
    }

    /* Process command */
    if(argc == 0) {
        /* Empty command line, skip */
    }
    /* Built in commands */
    else if(strcmp(argv[0], "cls") == 0) {
      /* Cls command: clear the screen */
      if(argc == 1) {
        result = syscall(SYSCALL_IO_CLEAR_SCREEN, 0);
      } else {
        putstr("usage: cls\n\r");
      }

    } else if(strcmp(argv[0], "list") == 0) {
      /* List command: list directory entries */

      /* If not path arg provided, add and implicit ROOT_DIR_NAME */
      /* This way it shows system disk contents */
      if(argc == 1) {
        argv[1] = ROOT_DIR_NAME;
        argc = 2;
      }

      if(argc == 2) {
        uchar line[64];
        struct SFS_ENTRY entry;

        /* Get number of entries in target dir */
        n = fs_list(&entry, argv[1], 0);
        if(n >= ERROR_ANY) {
          putstr("path not found\n\r");
          continue;
        }

        if(n > 0) {
          putstr("\n\r");

          /* Print one by one */
          for(i=0; i<n; i++) {
            struct TIME etime;
            uint c, size;

            /* Get entry */
            result = fs_list(&entry, argv[1], i);
            if(result >= ERROR_ANY) {
              putstr("Error\n\r");
              break;
            }

            /* Listed entry is a dir? If so,
             * start this line with a '+' */
            memset(line, 0, sizeof(line));
            strcpy_s(line, entry.flags & T_DIR ? "+ " : "  ", sizeof(line));
            strcat_s(line, entry.name, sizeof(line)); /* Append name */

            /* We want size to be right-aligned so add spaces
             * depending on figures of entry size */
            for(c=strlen(line); c<22; c++) {
              line[c] = ' ';
            }
            size = entry.size;
            while(size = size / 10 ) {
              line[--c] = 0;
            }

            /* Print name and size */
            putstr("%s%u %s   ", line, (uint)entry.size,
              (entry.flags & T_DIR) ? "items" : "bytes");

            /* Print date */
            fs_fstime_to_systime(entry.time, &etime);
            putstr("%d/%s%d/%s%d %s%d:%s%d:%s%d\n\r",
              etime.year,
              etime.month <10?"0":"", etime.month,
              etime.day   <10?"0":"", etime.day,
              etime.hour  <10?"0":"", etime.hour,
              etime.minute<10?"0":"", etime.minute,
              etime.second<10?"0":"", etime.second);
          }
          putstr("\n\r");
        }
      } else {
        putstr("usage: list <path>\n\r");
      }

    } else if(strcmp(argv[0], "makedir") == 0) {
      /* Makedir command */
      if(argc == 2) {
        result = fs_create_directory(argv[1]);
        if(result == ERROR_NOT_FOUND) {
          putstr("error: path not found\n\r");
        } else if(result == ERROR_EXISTS) {
          putstr("error: destination already exists\n\r");
        } else if(result == ERROR_NO_SPACE) {
          putstr("error: can't allocate destination in filesystem\n\r");
        } else if(result >= ERROR_ANY) {
          putstr("error: coludn't create directory\n\r");
        }
      } else {
        putstr("usage: makedir <path>\n\r");
      }

    } else if(strcmp(argv[0], "delete") == 0) {
      /* Delete command */
      if(argc == 2) {
        result = fs_delete(argv[1]);
        if(result >= ERROR_ANY) {
          putstr("error: failed to delete\n\r");
        }
      } else {
        putstr("usage: delete <path>\n\r");
      }

    } else if(strcmp(argv[0], "move") == 0) {
      /* Move command */
      if(argc == 3) {
        result = fs_move(argv[1], argv[2]);
        if(result == ERROR_NOT_FOUND) {
          putstr("error: path not found\n\r");
        } else if(result == ERROR_EXISTS) {
          putstr("error: destination already exists\n\r");
        } else if(result == ERROR_NO_SPACE) {
          putstr("error: can't allocate destination in filesystem\n\r");
        } else if(result >= ERROR_ANY) {
          putstr("error: coludn't move files\n\r");
        }
      } else {
        putstr("usage: move <path> <newpath>\n\r");
      }

    } else if(strcmp(argv[0], "copy") == 0) {
      /* Copy command */
      if(argc == 3) {
        result = fs_copy(argv[1], argv[2]);
        if(result == ERROR_NOT_FOUND) {
          putstr("error: path not found\n\r");
        } else if(result == ERROR_EXISTS) {
          putstr("error: destination already exists\n\r");
        } else if(result == ERROR_NO_SPACE) {
          putstr("error: can't allocate destination in filesystem\n\r");
        } else if(result >= ERROR_ANY) {
          putstr("error: coludn't copy files\n\r");
        }
      } else {
        putstr("usage: copy <srcpath> <dstpath>\n\r");
      }
    } else if(strcmp(argv[0], "info") == 0) {
      /* Info command: show system info */
      if(argc == 1) {
        putstr("\n\r");
        putstr("NANO S16 [Version 2.0 build 7]\n\r");
        putstr("\n\r");

        putstr("Disks:\n\r");
        fs_init_info(); /* Rescan disks */
        for(i=0; i<MAX_DISK; i++) {
          n = index_to_disk(i);
          if(disk_info[i].size) {
            putstr("%s %s(%UMB)   Disk size: %UMB\n\r",
              disk_to_string(n), disk_info[i].fstype == FS_TYPE_NSFS ? "NSFS" : "UNKN",
              (uint32_t)blocks_to_MB(disk_info[i].fssize), disk_info[i].size);
          }
        }
        putstr("\n\r");
        putstr("System disk: %s\n\r", disk_to_string(system_disk));
        putstr("Serial port status: %s\n\r", serial_status & 0x80 ? "Error" : "Enabled");
        putstr("A20 Line status: %s\n\r", a20_enabled ? "Enabled" : "Disabled");
        putstr("\n\r");
      } else {
        putstr("usage: info\n\r");
      }
    } else if(strcmp(argv[0], "clone") == 0) {
      /* Clone command: clone system disk in another disk */
      if(argc == 2) {
        struct SFS_ENTRY entry;
        uint disk, disk_index;
        uint sysdisk_index = disk_to_index(system_disk);

        /* Show source disk info */
        putstr("System disk: %s    fs=%s  size=%UMB\n\r",
          disk_to_string(system_disk),
          disk_info[sysdisk_index].fstype == FS_TYPE_NSFS ? "NSFS   " : "unknown",
          blocks_to_MB(disk_info[sysdisk_index].fssize));

        /* Check target disk */
        disk = string_to_disk(argv[1]);
        if(disk == ERROR_NOT_FOUND) {
          putstr("Target disk not found (%s)\n\r", argv[1]);
          continue;
        }
        if(disk == system_disk) {
          putstr("Target disk can't be the system disk\n\r");
          continue;
        }

        /* Show target disk info */
        disk_index = disk_to_index(disk);
        putstr("Target disk: %s    fs=%s  size=%UMB\n\r",
          disk_to_string(disk),
          disk_info[disk_index].fstype == FS_TYPE_NSFS ? "NSFS   " : "unknown",
          blocks_to_MB(disk_info[disk_index].fssize));

        putstr("\n\r");

        /* User should know this */
        putstr("Target disk (%s) will lose all data\n\r", disk_to_string(disk));
        putstr("Target disk (%s) will contain a %UMB NSFS filesystem after operation\n\r",
          disk_to_string(disk), disk_info[disk_index].size);

        /* Ask for confirmation */
        putstr("\n\r");
        putstr("Press 'y' to confirm: ");
        if(getLO(getkey(WAIT_KEY)) != 'y') {
          putstr("\n\rUser aborted operation\n\r");
          continue;
        }

        putstr("y\n\r");

        /* Format disk and copy kernel */
        putstr("Formatting and copying system files...\n\r");
        result = fs_format(disk);
        if(result != 0) {
          putstr("Error formatting disk. Aborted\n\r");
          continue;
        }

        /* Copy user files */
        putstr("Copying user files...\n\r");

        n = fs_list(&entry, ROOT_DIR_NAME, 0);
        if(n >= ERROR_ANY) {
          putstr("Error creating file list\n\r");
          continue;
        }

        /* List entries */
        for(i=0; i<n; i++) {
          uchar dst[64];
          result = fs_list(&entry, ROOT_DIR_NAME, i);
          if(result >= ERROR_ANY) {
            putstr("Error copying files. Aborted\n\r");
            break;
          }

          strcpy_s(dst, argv[1], sizeof(dst));
          strcat_s(dst, PATH_SEPARATOR_S, sizeof(dst));
          strcat_s(dst, entry.name, sizeof(dst));

          debugstr("copy %s %s\n\r", entry.name, dst);
          result = fs_copy(entry.name, dst);
          /* Skip ERROR_EXISTS errors, because system files were copied
           * by fs_format function, so they are expected to fail */
          if(result >= ERROR_ANY && result != ERROR_EXISTS) {
            putstr("Error copying %s. Aborted\n\r", entry.name);
            break;
          }
        }

        /* Notify result */
        if(result < ERROR_ANY) {
          putstr("Operation completed\n\r");
        }
      } else {
        putstr("usage: clone <target_disk>\n\r");
      }

    } else if(strcmp(argv[0], "read") == 0) {
      /* Read command: read a file */
      if(argc == 2) {
        uint offset = 0;
        uchar buff[128];
        memset(buff, 0, sizeof(buff));
        /* While it can read the file, print it */
        while(result = fs_read_file(buff, argv[1], offset, sizeof(buff))) {
          if(result >= ERROR_ANY) {
            putstr("\n\rThere was an error reading input file\n\r");
            break;
          }
          for(i=0; i<result; i++) {
            putchar(buff[i]);
            /* Add '\r' after '\n' chars */
            if(buff[i] == '\n') {
              putchar('\r');
            }
          }
          memset(buff, 0, sizeof(buff));
          offset += result;
        }
        putstr("\n\r");
      } else {
        putstr("usage: read <path>\n\r");
      }

    } else if(strcmp(argv[0], "time") == 0) {
      /* Time command: Show date and time */
      if(argc == 1) {
        struct TIME ctime;
        time(&ctime);

        putstr("\n\r%d/%s%d/%s%d %s%d:%s%d:%s%d\n\r\n\r",
          ctime.year,
          ctime.month <10?"0":"", ctime.month,
          ctime.day   <10?"0":"", ctime.day,
          ctime.hour  <10?"0":"", ctime.hour,
          ctime.minute<10?"0":"", ctime.minute,
          ctime.second<10?"0":"", ctime.second);
      } else if(argc == 3 && /* Easter egg */
        strcmp(argv[1], "of") == 0 &&
        strcmp(argv[2], "love") == 0) {
        putstr("\n\r2000/04/30 17:00:00\n\r\n\r");
      } else {
        putstr("usage: time\n\r");
      }

    } else if(strcmp(argv[0], "shutdown") == 0) {
      /* Shutdown command: Shutdown computer */
      if(argc == 1) {
        apm_shutdown();
        putstr("This computer does not support APM\n\r");
      } else {
        putstr("usage: shutdown\n\r");
      }

    } else if(strcmp(argv[0], "config") == 0) {
      /* Time command: Show date and time */
      if(argc == 1) {
        putstr("\n\r");
        putstr("debug: %s       - output debug info through serial port\n\r", serial_debug ? " enabled" : "disabled");
        putstr("\n\r");
      } else if(argc == 3 && /* Easter egg */
        strcmp(argv[1], "debug") == 0) {
          if(strcmp(argv[2], "enabled") == 0) {
            serial_debug = 1;
          } else if(strcmp(argv[2], "disabled") == 0) {
            serial_debug = 0;
          } else {
            putstr("Invalid value. Valid values are: enabled, disabled\n\r");
          }
      } else {
        putstr("usages:\n\rconfig\n\rconfig <debug> <enabled|disabled>");
      }

    } else if(strcmp(argv[0], "help") == 0) {
      /* Help command: Show help, and some tricks */
      if(argc == 1) {
        putstr("\n\r");
        putstr("Built-in commands:\n\r");
        putstr("\n\r");
        putstr("clone    - clone system in another disk\n\r");
        putstr("cls      - clear the screen\n\r");
        putstr("config   - show or set config\n\r");
        putstr("copy     - create a copy of a file or directory\n\r");
        putstr("delete   - delete entry\n\r");
        putstr("help     - show this help\n\r");
        putstr("info     - show system info\n\r");
        putstr("list     - list directory contents\n\r");
        putstr("makedir  - create directory\n\r");
        putstr("move     - move file or directory\n\r");
        putstr("read     - show file contents in screen\n\r");
        putstr("shutdown - shutdown the computer\n\r");
        putstr("time     - show time and date\n\r");
        putstr("\n\r");
      } else if(argc == 2 &&  /* Easter egg */
        (strcmp(argv[1], "huri") == 0 || strcmp(argv[1], "marylin") == 0)) {
        putstr("\n\r");
        putstr("                                     _,-/\\^---,      \n\r");
        putstr("             ;\"~~~~~~~~\";          _/;; ~~  {0 `---v \n\r");
        putstr("           ;\" :::::   :: \"\\_     _/   ;;     ~ _../  \n\r");
        putstr("         ;\" ;;    ;;;       \\___/::    ;;,'~~~~      \n\r");
        putstr("       ;\"  ;;;;.    ;;     ;;;    ::   ,/            \n\r");
        putstr("      / ;;   ;;;______;;;;  ;;;    ::,/              \n\r");
        putstr("     /;;V_;; _-~~~~~~~~~~;_  ;;;   ,/                \n\r");
        putstr("    | :/ / ,/              \\_  ~~)/                  \n\r");
        putstr("    |:| / /~~~=              \\;; \\~~=                \n\r");
        putstr("    ;:;{::~~~~~~=              \\__~~~=               \n\r");
        putstr(" ;~~:;  ~~~~~~~~~               ~~~~~~               \n\r");
        putstr(" \\/~~                                               \n\r");
        putstr("\n\r");
      } else {
        putstr("usage: help\n\r");
      }

    } else {
      /* Not a built-in command */
      /* Try to find an executable file */
      uchar* prog_ext = ".bin";
      struct SFS_ENTRY entry;
      uchar prog_file_name[32];
      strcpy_s(prog_file_name, argv[0], sizeof(prog_file_name));

      /* Append .bin if there is not a '.' in the name*/
      if(!strchr(prog_file_name, '.')) {
        strcat_s(prog_file_name, prog_ext, sizeof(prog_file_name));
      }

      /* Find .bin file */
      result = fs_get_entry(&entry, prog_file_name, UNKNOWN_VALUE, UNKNOWN_VALUE);
      if(result < ERROR_ANY) {
        /* Found */
        if(entry.flags & T_FILE) {
          /* It's a file: load it */
          result = fs_read_file(EXTERN_PROGRAM_MEMLOC, prog_file_name, 0,
            min((uint)entry.size, 0xFFFF - EXTERN_PROGRAM_MEMLOC));
        } else {
          /* It's not a file: error */
          result = ERROR_NOT_FOUND;
        }
      }

      if(result >= ERROR_ANY || result == 0) {
        putstr("unkown command\n\r");
      } else {
        extern_main* m = EXTERN_PROGRAM_MEMLOC;

        /* Check name ends with ".bin" */
        if(strcmp(&prog_file_name[strchr(prog_file_name, '.') - 1], prog_ext)) {
          putstr("error: only %s files can be executed\n\r", prog_ext);
          continue;
        }

        debugstr("CLI: Running program %s (%d bytes)\n\r",
          prog_file_name, (uint)entry.size);

        /* Run program */
        m(argc, argv);

        /* Restore environment */
        set_show_cursor(SHOW_CURSOR); /* The program could have hidden it */
      }
    }
  }

  /* This code should never be executed */
  /* Halt the computer */
  halt();
}
