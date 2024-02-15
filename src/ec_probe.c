#define _XOPEN_SOURCE  500 // unistd.h: export pwrite()/pread()
#define _DEFAULT_SOURCE    // endian.h:

#include "nbfc.h"
#include "macros.h"
#include "sleep.h"
#include "ec_linux.h"
#include "ec_sys_linux.h"
#include "model_config.h"
#include "optparse/optparse.h"
#include "parse_number.h"
#include "generated/ec_probe.help.h"
#include "program_name.c"
#include "log.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>

#include "error.c"             // src
#include "ec.c"                // src
#include "ec_linux.c"          // src
#include "ec_sys_linux.c"      // src
#include "optparse/optparse.c" // src
#include "memory.c"            // src
#include "nxjson.c"            // src
#include "model_config.c"      // src

#define Console_Black       "\033[0;30m"
#define Console_Red         "\033[0;31m"
#define Console_Green       "\033[0;32m"
#define Console_Yelllow     "\033[0;33m"
#define Console_Blue        "\033[0;34m"
#define Console_Magenta     "\033[0;35m"
#define Console_Cyan        "\033[0;36m"
#define Console_White       "\033[0;37m"
#define Console_Gray        "\033[0;38m"

#define Console_BoldBlack   "\033[1;30m"
#define Console_BoldRed     "\033[1;31m"
#define Console_BoldGreen   "\033[1;32m"
#define Console_BoldYelllow "\033[1;33m"
#define Console_BoldBlue    "\033[1;34m"
#define Console_BoldMagenta "\033[1;35m"
#define Console_BoldCyan    "\033[1;36m"
#define Console_BoldWhite   "\033[1;37m"
#define Console_BoldGray    "\033[1;38m"

#define Console_Reset       "\033[0;0m"
#define Console_Clear       "\033[1;1H\033[2J"

#define             RegistersSize 256
typedef uint8_t     RegisterBuf[RegistersSize];
typedef const char* RegisterColors[RegistersSize];
static RegisterBuf  Registers_Log[32768];

static void         Register_PrintRegister(RegisterBuf*, RegisterColors);
static inline void  Register_FromEC(RegisterBuf*);
static void         Register_PrintWatch(RegisterBuf*, RegisterBuf*, RegisterBuf*);
static void         Register_PrintMonitor(RegisterBuf*, int);
static void         Register_WriteMonitorReport(RegisterBuf*, int, FILE*);
static void         Register_PrintDump(RegisterBuf*);
static void         Handle_Signal(int);

static EC_VTable*   ec;
static volatile int quit;

static int Read();
static int Write();
static int Dump();
static int Monitor();
static int Watch();

enum Command {
  Command_Read,
  Command_Write,
  Command_Dump,
  Command_Monitor,
  Command_Watch,
  Command_Help,
};

static enum Command Command_From_String(const char* s) {
  const char* cmds[] = { "read", "write", "dump", "monitor", "watch", "help" };

  for (int i = 0; i < ARRAY_SSIZE(cmds); ++i)
    if (!strcmp(cmds[i], s))
      return (enum Command) i;

  return (enum Command) -1;
}

static const char* HelpTexts[] = {
  EC_PROBE_READ_HELP_TEXT,
  EC_PROBE_WRITE_HELP_TEXT,
  EC_PROBE_DUMP_HELP_TEXT,
  EC_PROBE_MONITOR_HELP_TEXT,
  EC_PROBE_WATCH_HELP_TEXT,
  EC_PROBE_HELP_TEXT,
};

static const cli99_option main_options[] = {
  {"-e|--embedded-controller", -'e',  1},
  {"-h|--help",                -'h',  0},
  {"--version",                -'V',  0},
  {"command",                   'C',  1|cli99_required_option},
  cli99_options_end()
};

static const cli99_option read_command_options[] = {
  cli99_include_options(&main_options),
  {"register",                  'R',  1|cli99_required_option},
  {"-w|--word",                -'w',  0},
  cli99_options_end()
};

static const cli99_option write_command_options[] = {
  cli99_include_options(&main_options),
  {"register",                  'R',  1|cli99_required_option},
  {"value",                     'V',  1|cli99_required_option},
  {"-w|--word",                -'w',  0},
  cli99_options_end()
};

static const cli99_option monitor_command_options[] = {
  cli99_include_options(&main_options),
  {"-r|--report",              -'r',  1},
  {"-c|--clearly",             -'c',  0},
  {"-d|--decimal",             -'d',  0},
  {"-t|--timespan",            -'t',  1},
  {"-i|--interval",            -'i',  1},
  cli99_options_end()
};

static const cli99_option watch_command_options[] = {
  cli99_include_options(&main_options),
  {"-t|--timespan",            -'t',  1},
  {"-i|--interval",            -'i',  1},
  cli99_options_end()
};

static const cli99_option* Options[] = {
  read_command_options,
  write_command_options,
  main_options, // dump
  monitor_command_options,
  watch_command_options,
  main_options, // help
};

static struct {
  int         timespan;
  int         interval;
  const char* report;
  bool        clearly;
  bool        decimal;
  uint8_t     register_;
  uint16_t    value;
  bool        use_word;
} options = {0};

int main(int argc, char* const argv[]) {
  Program_Name_Set(argv[0]);

  options.interval = 500;
  ec = NULL;
  enum Command cmd = Command_Help;

  cli99 p;
  cli99_Init(&p, argc, argv, main_options, cli99_options_python);

  int o;
  char* err;
  while ((o = cli99_GetOpt(&p))) {
    switch (o) {
    case 'C':
      cmd = Command_From_String(p.optarg);

      if (cmd == (enum Command) -1) {
        Log_Error("Invalid command: %s\n", p.optarg);
        return NBFC_EXIT_CMDLINE;
      }

      if (cmd == Command_Help) {
        printf(EC_PROBE_HELP_TEXT, argv[0]);
        return 0;
      }

      cli99_SetOptions(&p, Options[cmd], false);
      break;
    case 'R':
      options.register_ = parse_number(p.optarg, 0, 255, &err);
      if (err) {
        Log_Error("Register: %s: %s\n", p.optarg, err);
        return NBFC_EXIT_CMDLINE;
      }
      break;
    case 'V':
      options.value = parse_number(p.optarg, 0, 65535, &err);
      if (err) {
        Log_Error("Value: %s: %s\n", p.optarg, err);
        return NBFC_EXIT_CMDLINE;
      }
      break;
    case -'h':  printf(HelpTexts[cmd], argv[0]);         return 0;
    case -'V':  printf("ec_probe " NBFC_VERSION "\n");   return 0;
    case -'c':  options.clearly  = 1;                    break;
    case -'d':  options.decimal  = 1;                    break;
    case -'w':  options.use_word = 1;                    break;
    case -'r':  options.report   = p.optarg;             break;
    case -'e':
      switch(EmbeddedControllerType_FromString(p.optarg)) {
        case EmbeddedControllerType_ECSysLinux:     ec = &EC_SysLinux_VTable;      break;
        case EmbeddedControllerType_ECSysLinuxACPI: ec = &EC_SysLinux_ACPI_VTable; break;
        case EmbeddedControllerType_ECLinux:        ec = &EC_Linux_VTable;         break;
        default:
          Log_Error("-e|--embedded-controller: Invalid value: %s\n", p.optarg);
          return NBFC_EXIT_CMDLINE;
      }
      break;
    case -'t':
      options.timespan = parse_number(p.optarg, 1, INT64_MAX, &err);
      options.timespan *= 1000;
      if (err) {
        Log_Error("-t|--timespan: %s: %s\n", p.optarg, err);
        return NBFC_EXIT_CMDLINE;
      }
      break;
    case -'i':
      options.interval = parse_number(p.optarg, 1, INT64_MAX, &err);
      options.interval *= 1000;
      if (err) {
        Log_Error("-i|--interval: %s: %s\n", p.optarg, err);
        return NBFC_EXIT_CMDLINE;
      }
      break;
    default:
      cli99_ExplainError(&p);
      return NBFC_EXIT_CMDLINE;
    }
  }

  if (cmd == Command_Help) {
    printf(EC_PROBE_HELP_TEXT, argv[0]);
    return 0;
  }

  if (! cli99_End(&p)) {
    Log_Error("Too much arguments\n");
    return NBFC_EXIT_CMDLINE;
  }

  if (! cli99_CheckRequired(&p)) {
    cli99_ExplainError(&p);
    return NBFC_EXIT_CMDLINE;
  }

  if (geteuid()) {
    Log_Error("This program must be run as root\n");
    return NBFC_EXIT_FAILURE;
  }

  signal(SIGINT,  Handle_Signal);
  signal(SIGTERM, Handle_Signal);

  if (ec == NULL) {
    Error* e = EC_FindWorking(&ec);
    e_die();
  }

  Error* e = ec->Open();
  e_die();

  switch (cmd) {
    case Command_Dump:    return Dump();
    case Command_Read:    return Read();
    case Command_Write:   return Write();
    case Command_Monitor: return Monitor();
    case Command_Watch:   return Watch();
    default: break;
  }

  return 0;
}

static int Read() {
  if (options.use_word) {
    uint16_t word;
    Error* e = ec->ReadWord(options.register_, &word);
    e_die();
    printf("%d (%.2X)\n", word, word);
  }
  else {
    uint8_t byte;
    Error* e = ec->ReadByte(options.register_, &byte);
    e_die();
    printf("%d (%.2X)\n", byte, byte);
  }

  return 0;
}

static int Write() {
  if (options.use_word) {
    Error* e = ec->WriteWord(options.register_, options.value);
    e_die();
  }
  else {
    if (options.value > 255) {
      Log_Error("write: Value too big: %d\n", options.value);
      return NBFC_EXIT_CMDLINE;
    }
    Error* e = ec->WriteByte(options.register_, options.value);
    e_die();
  }

  return 0;
}

static int Dump() {
  RegisterBuf register_buf;
  Register_FromEC(&register_buf);
  Register_PrintDump(&register_buf);
  return 0;
}

static int Monitor() {
  const int max_loops = (!options.timespan) ? INT_MAX :
    (options.timespan / options.interval);

  RegisterBuf* regs = Registers_Log;
  int size = ARRAY_SSIZE(Registers_Log);
  int loops;
  for (loops = 0; !quit && loops < max_loops && --size; ++loops) {
    Register_FromEC(regs + loops);
    Register_PrintMonitor(regs, loops);
    sleep_ms(options.interval);
  }

  if (options.report) {
    FILE* fh = fopen(options.report, "w");
    if (! fh) {
      Log_Error("%s: %s\n", options.report, strerror(errno));
      return NBFC_EXIT_FAILURE;
    }
    Register_WriteMonitorReport(regs, loops, fh);
    fclose(fh);
  }

  return 0;
}

static int Watch() {
  const int max_loops = (!options.timespan) ? INT_MAX :
    (options.timespan / options.interval);

  int size = ARRAY_SSIZE(Registers_Log);
  RegisterBuf* regs = Registers_Log;
  Register_FromEC(regs);
  for (int loops = 1; !quit && loops < max_loops && --size; ++loops) {
    Register_FromEC(regs + loops);
    Register_PrintWatch(regs , regs + loops, regs + loops - 1);
    sleep_ms(options.interval);
  }

  return 0;
}

static void Handle_Signal(int sig) {
  quit = sig;
}

// ============================================================================
// Registers code
// ============================================================================

static void Register_PrintRegister(RegisterBuf* self, RegisterColors color) {
  printf(Console_Reset
    "---|------------------------------------------------\n"
    "   | 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n"
    "---|------------------------------------------------\n");

  for (int i = 0; i <= 0xF0; i += 0x10) {
    printf(Console_Reset "%.2X |", i);
    if (color)
      for (int j = 0; j <= 0xF; ++j)
        printf("%s %.2X", color[i + j], my[i + j]);
    else
      for (int j = 0; j <= 0xF; ++j)
        printf(" %.2X", my[i + j]);
    printf("\n");
  }
}

static inline void Register_FromEC(RegisterBuf* self) {
  for (int i = 0; i < RegistersSize; i++)
    ec->ReadByte(i, &my[i]);
}

static void Register_PrintWatch(RegisterBuf* all_readings, RegisterBuf* current, RegisterBuf* previous) {
  RegisterColors colors;

  for (int register_ = 0; register_ < RegistersSize; ++register_) {
    const uint8_t byte = (*current)[register_];
    const uint8_t diff = byte - (*previous)[register_];
    bool has_changed = false;

    uint8_t save = byte;
    for (range(RegisterBuf*, r, all_readings, previous)) {
      if (save != (*r)[register_]) {
        has_changed = true;
        break;
      }
    }

    /**/ if (diff)          colors[register_] = Console_Yelllow;
    else if (has_changed)   colors[register_] = Console_BoldBlue;
    else if (byte == 0xFF)  colors[register_] = Console_White;
    else if (byte)          colors[register_] = Console_BoldWhite;
    else                    colors[register_] = Console_BoldBlack;
  }

  Register_PrintRegister(current, colors);
}

static void Register_PrintMonitor(RegisterBuf* readings, int size) {
  printf(Console_Clear);

  for (int register_ = 0; register_ < RegistersSize; ++register_) {
    bool register_has_changed = false;
    for (range(int, i, 0, size)) {
      if (readings[0][register_] != readings[i][register_]) {
        register_has_changed = true;
        break;
      }
    }

    if (! register_has_changed)
      continue;

    printf(Console_Green "0x%.2X:", register_);
    uint8_t byte = readings[0][register_];
    for (range(int, i, max(size - 24, 0), size)) {
      const uint8_t diff = byte - readings[i][register_];
      byte = readings[i][register_];
      if (diff)
        printf(Console_BoldBlue " %.2X", byte);
      else
        printf(Console_BoldWhite " %.2X", byte);
    }
    printf("\n");
  }
}

static void Register_WriteMonitorReport(RegisterBuf* readings, int size, FILE* fh) {
  for (int register_ = 0; register_ < RegistersSize; ++register_) {
    bool register_has_changed = false;
    for (range(int, i, 0, size)) {
      if (readings[0][register_] != readings[i][register_]) {
        register_has_changed = true;
        break;
      }
    }

    if (! register_has_changed)
      continue;

    fprintf(fh, "%.2X", register_);
    for (range(int, i, 0, size)) {
      if (options.clearly && i > 0 && readings[i][register_] == readings[i - 1][register_])
        continue;

      if (options.decimal)
        fprintf(fh, ",%d", readings[i][register_]);
      else
        fprintf(fh, ",%.2X", readings[i][register_]);
    }
    fprintf(fh, "\n");
  }
}

static void Register_PrintDump(RegisterBuf* self) {
  RegisterColors colors;

  for (int i = 0; i < RegistersSize; ++i)
    colors[i] = (my[i] == 0x00 ? Console_BoldBlack :
                 my[i] == 0xFF ? Console_BoldGreen :
                                 Console_BoldBlue);

  Register_PrintRegister(self, colors);
  printf("%s", Console_Reset);
}

