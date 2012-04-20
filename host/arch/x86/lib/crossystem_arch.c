/* Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <linux/nvram.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "host_common.h"

#include "crossystem.h"
#include "crossystem_arch.h"
#include "utility.h"
#include "vboot_common.h"
#include "vboot_nvstorage.h"
#include "vboot_struct.h"


/* ACPI constants from Chrome OS Main Processor Firmware Spec */
/* Boot reasons from BINF.0, from early H2C firmware */
/* Unknown */
#define BINF0_UNKNOWN                  0
/* Normal boot to Chrome OS */
#define BINF0_NORMAL                   1
/* Developer mode boot (developer mode warning displayed) */
#define BINF0_DEVELOPER                2
/* Recovery initiated by user, using recovery button */
#define BINF0_RECOVERY_BUTTON          3
/* Recovery initiated by user pressing a key at developer mode warning
 * screen */
#define BINF0_RECOVERY_DEV_SCREEN_KEY  4
/* Recovery caused by BIOS failed signature check (neither rewritable
 * firmware was valid) */
#define BINF0_RECOVERY_RW_FW_BAD       5
/* Recovery caused by no OS kernel detected */
#define BINF0_RECOVERY_NO_OS           6
/* Recovery caused by OS kernel failed signature check */
#define BINF0_RECOVERY_BAD_OS          7
/* Recovery initiated by OS */
#define BINF0_RECOVERY_OS_INITIATED    8
/* OS-initiated S3 diagnostic path (debug mode boot) */
#define BINF0_S3_DIAGNOSTIC_PATH       9
/* S3 resume failed */
#define BINF0_S3_RESUME_FAILED        10
/* Recovery caused by TPM error */
#define BINF0_RECOVERY_TPM_ERROR      11
/* CHSW bitflags */
#define CHSW_RECOVERY_BOOT     0x00000002
#define CHSW_RECOVERY_EC_BOOT  0x00000004
#define CHSW_DEV_BOOT          0x00000020
#define CHSW_WP_BOOT           0x00000200
/* CMOS reboot field bitflags */
#define CMOSRF_RECOVERY        0x80
#define CMOSRF_DEBUG_RESET     0x40
#define CMOSRF_TRY_B           0x20
/* GPIO signal types */
#define GPIO_SIGNAL_TYPE_RECOVERY 1
#define GPIO_SIGNAL_TYPE_DEV 2
#define GPIO_SIGNAL_TYPE_WP 3

/* Base name for ACPI files */
#define ACPI_BASE_PATH "/sys/devices/platform/chromeos_acpi"
/* Paths for frequently used ACPI files */
#define ACPI_BINF_PATH ACPI_BASE_PATH "/BINF"
#define ACPI_CHNV_PATH ACPI_BASE_PATH "/CHNV"
#define ACPI_CHSW_PATH ACPI_BASE_PATH "/CHSW"
#define ACPI_FMAP_PATH ACPI_BASE_PATH "/FMAP"
#define ACPI_GPIO_PATH ACPI_BASE_PATH "/GPIO"
#define ACPI_VBNV_PATH ACPI_BASE_PATH "/VBNV"
#define ACPI_VDAT_PATH ACPI_BASE_PATH "/VDAT"

/* Base name for GPIO files */
#define GPIO_BASE_PATH "/sys/class/gpio"
#define GPIO_EXPORT_PATH GPIO_BASE_PATH "/export"

/* Filename for NVRAM file */
#define NVRAM_PATH "/dev/nvram"

/* Filename for legacy firmware update tries */
#define NEED_FWUPDATE_PATH "/mnt/stateful_partition/.need_firmware_update"

/* Filenames for PCI Vendor and Device IDs */
#define PCI_VENDOR_ID_PATH "/sys/bus/pci/devices/0000:00:00.0/vendor"
#define PCI_DEVICE_ID_PATH "/sys/bus/pci/devices/0000:00:00.0/device"

typedef struct PlatformFamily {
  unsigned int vendor;          /* Vendor id value */
  unsigned int device;          /* Device id value */
  const char* platform_string; /* String to return */
} PlatformFamily;

/* Array of platform family names, terminated with a NULL entry */
const PlatformFamily platform_family_array[] = {
  {0x8086, 0xA010, "PineTrail"},
  {0x8086, 0x3406, "Westmere"},
  {0x8086, 0x0104, "SandyBridge"}, /* mobile */
  {0x8086, 0x0100, "SandyBridge"}, /* desktop */
  {0x8086, 0x0154, "IvyBridge"},   /* mobile */
  {0x8086, 0x0150, "IvyBridge"},   /* desktop */
  /* Terminate with NULL entry */
  {NULL, NULL, NULL}
};

static void VbFixCmosChecksum(FILE* file) {
  int fd = fileno(file);
  ioctl(fd, NVRAM_SETCKS);
}


static int VbCmosRead(int offs, size_t size, void *ptr) {
  size_t res;
  FILE* f;

  f = fopen(NVRAM_PATH, "rb");
  if (!f)
    return -1;

  if (0 != fseek(f, offs, SEEK_SET)) {
    fclose(f);
    return -1;
  }

  res = fread(ptr, size, 1, f);
  if (1 != res && errno == EIO && ferror(f)) {
    VbFixCmosChecksum(f);
    res = fread(ptr, size, 1, f);
  }

  fclose(f);
  return (1 == res) ? 0 : -1;
}


static int VbCmosWrite(int offs, size_t size, const void *ptr) {
  size_t res;
  FILE* f;

  f = fopen(NVRAM_PATH, "w+b");
  if (!f)
    return -1;

  if (0 != fseek(f, offs, SEEK_SET)) {
    fclose(f);
    return -1;
  }

  res = fwrite(ptr, size, 1, f);
  if (1 != res && errno == EIO && ferror(f)) {
    VbFixCmosChecksum(f);
    res = fwrite(ptr, size, 1, f);
  }

  fclose(f);
  return (1 == res) ? 0 : -1;
}


int VbReadNvStorage(VbNvContext* vnc) {
  int offs;

  /* Get the byte offset from VBNV */
  offs = ReadFileInt(ACPI_VBNV_PATH ".0");
  if (offs == -1)
    return -1;
  if (VBNV_BLOCK_SIZE > ReadFileInt(ACPI_VBNV_PATH ".1"))
    return -1;  /* NV storage block is too small */

  if (0 != VbCmosRead(offs, VBNV_BLOCK_SIZE, vnc->raw))
    return -1;

  return 0;
}


int VbWriteNvStorage(VbNvContext* vnc) {
  int offs;

  if (!vnc->raw_changed)
    return 0;  /* Nothing changed, so no need to write */

  /* Get the byte offset from VBNV */
  offs = ReadFileInt(ACPI_VBNV_PATH ".0");
  if (offs == -1)
    return -1;
  if (VBNV_BLOCK_SIZE > ReadFileInt(ACPI_VBNV_PATH ".1"))
    return -1;  /* NV storage block is too small */

  if (0 != VbCmosWrite(offs, VBNV_BLOCK_SIZE, vnc->raw))
    return -1;

  return 0;
}


/*
 * Get buffer data from ACPI.
 *
 * Buffer data is expected to be represented by a file which is a text dump of
 * the buffer, representing each byte by two hex numbers, space and newline
 * separated.
 *
 * On success, stores the amount of data read in bytes to *buffer_size; on
 * erros, sets *buffer_size=0.
 *
 * Input - ACPI file name to get data from.
 *
 * Output: a pointer to AcpiBuffer structure containing the binary
 *         representation of the data. The caller is responsible for
 *         deallocating the pointer, this will take care of both the structure
 *         and the buffer. Null in case of error.
 */
static uint8_t* VbGetBuffer(const char* filename, int* buffer_size) {
  FILE* f = NULL;
  char* file_buffer = NULL;
  uint8_t* output_buffer = NULL;
  uint8_t* return_value = NULL;

  /* Assume error until proven otherwise */
  if (buffer_size)
    *buffer_size = 0;

  do {
    struct stat fs;
    uint8_t* output_ptr;
    int rv, i, real_size;
    int parsed_size = 0;

    rv = stat(filename, &fs);
    if (rv || !S_ISREG(fs.st_mode))
      break;

    f = fopen(filename, "r");
    if (!f)
      break;

    file_buffer = malloc(fs.st_size + 1);
    if (!file_buffer)
      break;

    real_size = fread(file_buffer, 1, fs.st_size, f);
    if (!real_size)
      break;
    file_buffer[real_size] = '\0';

    /* Each byte in the output will replace two characters and a space
     * in the input, so the output size does not exceed input side/3
     * (a little less if account for newline characters). */
    output_buffer = malloc(real_size/3);
    if (!output_buffer)
      break;
    output_ptr = output_buffer;

    /* process the file contents */
    for (i = 0; i < real_size; i++) {
      char* base, *end;

      base = file_buffer + i;

      if (!isxdigit(*base))
        continue;

      output_ptr[parsed_size++] = strtol(base, &end, 16) & 0xff;

      if ((end - base) != 2)
        /* Input file format error */
        break;

      i += 2; /* skip the second character and the following space */
    }

    if (i == real_size) {
      /* all is well */
      return_value = output_buffer;
      output_buffer = NULL; /* prevent it from deallocating */
      if (buffer_size)
        *buffer_size = parsed_size;
    }
  } while(0);

  /* wrap up */
  if (f)
    fclose(f);

  if (file_buffer)
    free(file_buffer);

  if (output_buffer)
    free(output_buffer);

  return return_value;
}


VbSharedDataHeader* VbSharedDataRead(void) {
  VbSharedDataHeader* sh;
  int got_size = 0;
  int expect_size;

  sh = (VbSharedDataHeader*)VbGetBuffer(ACPI_VDAT_PATH, &got_size);
  if (!sh)
    return NULL;

  /* Make sure the size is sufficient for the struct version we got.
   * Check supported old versions first. */
  if (1 == sh->struct_version)
    expect_size = VB_SHARED_DATA_HEADER_SIZE_V1;
  else {
    /* There'd better be enough data for the current header size. */
    expect_size = sizeof(VbSharedDataHeader);
  }

  if (got_size < expect_size) {
    free(sh);
    return NULL;
  }
  if (sh->data_size > got_size)
    sh->data_size = got_size;  /* Truncated read */

  return sh;
}


/* Read the CMOS reboot field in NVRAM.
 *
 * Returns 0 if the mask is clear in the field, 1 if set, or -1 if error. */
static int VbGetCmosRebootField(uint8_t mask) {
  int chnv;
  uint8_t nvbyte;

  /* Get the byte offset from CHNV */
  chnv = ReadFileInt(ACPI_CHNV_PATH);
  if (chnv == -1)
    return -1;

  if (0 != VbCmosRead(chnv, 1, &nvbyte))
    return -1;

  return (nvbyte & mask ? 1 : 0);
}


/* Write the CMOS reboot field in NVRAM.
 *
 * Sets (value=0) or clears (value!=0) the mask in the byte.
 *
 * Returns 0 if success, or -1 if error. */
static int VbSetCmosRebootField(uint8_t mask, int value) {
  int chnv;
  uint8_t nvbyte;

  /* Get the byte offset from CHNV */
  chnv = ReadFileInt(ACPI_CHNV_PATH);
  if (chnv == -1)
    return -1;

  if (0 != VbCmosRead(chnv, 1, &nvbyte))
    return -1;

  /* Set/clear the mask */
  if (value)
    nvbyte |= mask;
  else
    nvbyte &= ~mask;

  /* Write the byte back */
  if (0 != VbCmosWrite(chnv, 1, &nvbyte))
    return -1;

  /* Success */
  return 0;
}


/* Read the active main firmware type into the destination buffer.
 * Passed the destination and its size.  Returns the destination, or
 * NULL if error. */
static const char* VbReadMainFwType(char* dest, int size) {

  /* Try reading type from BINF.3 */
  switch(ReadFileInt(ACPI_BINF_PATH ".3")) {
    case BINF3_NETBOOT:
      return StrCopy(dest, "netboot", size);
    case BINF3_RECOVERY:
      return StrCopy(dest, "recovery", size);
    case BINF3_NORMAL:
      return StrCopy(dest, "normal", size);
    case BINF3_DEVELOPER:
      return StrCopy(dest, "developer", size);
    default:
      break;  /* Fall through to legacy handling */
  }

  /* Fall back to BINF.0 for legacy systems like Mario. */
  switch(ReadFileInt(ACPI_BINF_PATH ".0")) {
    case -1:
      /* Both BINF.0 and BINF.3 are missing, so this isn't Chrome OS
       * firmware. */
      return StrCopy(dest, "nonchrome", size);
    case BINF0_NORMAL:
      return StrCopy(dest, "normal", size);
    case BINF0_DEVELOPER:
      return StrCopy(dest, "developer", size);
    case BINF0_RECOVERY_BUTTON:
    case BINF0_RECOVERY_DEV_SCREEN_KEY:
    case BINF0_RECOVERY_RW_FW_BAD:
    case BINF0_RECOVERY_NO_OS:
    case BINF0_RECOVERY_BAD_OS:
    case BINF0_RECOVERY_OS_INITIATED:
    case BINF0_RECOVERY_TPM_ERROR:
      /* Assorted flavors of recovery boot reason. */
      return StrCopy(dest, "recovery", size);
    default:
      /* Other values don't map cleanly to firmware type. */
      return NULL;
  }
}


/* Read the recovery reason.  Returns the reason code or -1 if error. */
static int VbGetRecoveryReason(void) {
  VbSharedDataHeader* sh;
  int value = -1;

  /* Try reading from VbSharedData first */
  sh = VbSharedDataRead();
  if (sh) {
    if (sh->struct_version >= 2)
      value = sh->recovery_reason;
    free(sh);
    if (-1 != value)
      return value;
  }

  /* Try reading type from BINF.4 */
  value = ReadFileInt(ACPI_BINF_PATH ".4");
  if (-1 != value)
    return value;

  /* Fall back to BINF.0 for legacy systems like Mario. */
  switch(ReadFileInt(ACPI_BINF_PATH ".0")) {
    case BINF0_NORMAL:
    case BINF0_DEVELOPER:
      return VBNV_RECOVERY_NOT_REQUESTED;
    case BINF0_RECOVERY_BUTTON:
      return VBNV_RECOVERY_RO_MANUAL;
    case BINF0_RECOVERY_DEV_SCREEN_KEY:
      return VBNV_RECOVERY_RW_DEV_SCREEN;
    case BINF0_RECOVERY_RW_FW_BAD:
      return VBNV_RECOVERY_RO_INVALID_RW;
    case BINF0_RECOVERY_NO_OS:
      return VBNV_RECOVERY_RW_NO_OS;
    case BINF0_RECOVERY_BAD_OS:
      return VBNV_RECOVERY_RW_INVALID_OS;
    case BINF0_RECOVERY_OS_INITIATED:
      return VBNV_RECOVERY_LEGACY;
    default:
      /* Other values don't map cleanly to firmware type. */
      return -1;
  }
}

/* Determine the platform family and return it in the dest string.
 * This uses the PCI Bus 0, Device 0, Function 0 vendor and device id values
 * taken from sysfs to determine the platform family. This assumes there will
 * be a unique pair of values here for any given platform.
 */
static char* ReadPlatformFamilyString(char* dest, int size) {
  FILE* f;
  const PlatformFamily* p;
  unsigned int v = 0xFFFF;
  unsigned int d = 0xFFFF;

  f = fopen(PCI_VENDOR_ID_PATH, "rt");
  if (!f)
    return NULL;
  if(fscanf(f, "0x%4x", &v) != 1)
    return NULL;
  fclose(f);

  f = fopen(PCI_DEVICE_ID_PATH, "rt");
  if (!f)
    return NULL;
  if(fscanf(f, "0x%4x", &d) != 1)
    return NULL;
  fclose(f);

  for (p = platform_family_array; p->vendor; p++) {
    if((v == p->vendor) && (d == p->device))
      return StrCopy(dest, p->platform_string, size);
  }

  /* No recognized platform family was found */
  return NULL;
}

/* Physical GPIO number <N> may be accessed through /sys/class/gpio/gpio<M>/,
 * but <N> and <M> may differ by some offset <O>. To determine that constant,
 * we look for a directory named /sys/class/gpio/gpiochip<O>/. If there's not
 * exactly one match for that, we're SOL.
 */
static int FindGpioChipOffset(int *offset) {
  DIR *dir;
  struct dirent *ent;
  int match = 0;

  dir = opendir(GPIO_BASE_PATH);
  if (!dir) {
    return 0;
  }

  while(0 != (ent = readdir(dir))) {
    if (1 == sscanf(ent->d_name, "gpiochip%d", offset)) {
      match++;
    }
  }

  closedir(dir);
  return (1 == match);
}


/* Read a GPIO of the specified signal type (see ACPI GPIO SignalType).
 *
 * Returns 1 if the signal is asserted, 0 if not asserted, or -1 if error. */
static int ReadGpio(int signal_type) {
  char name[128];
  int index = 0;
  int gpio_type;
  int active_high;
  int controller_num;
  int controller_offset = 0;
  char controller_name[128];
  int value;

  /* Scan GPIO.* to find a matching signal type */
  for (index = 0; ; index++) {
    snprintf(name, sizeof(name), "%s.%d/GPIO.0", ACPI_GPIO_PATH, index);
    gpio_type = ReadFileInt(name);
    if (gpio_type == signal_type)
      break;
    else if (gpio_type == -1)
      return -1;                  /* Ran out of GPIOs before finding a match */
  }

  /* Read attributes and controller info for the GPIO */
  snprintf(name, sizeof(name), "%s.%d/GPIO.1", ACPI_GPIO_PATH, index);
  active_high = ReadFileBit(name, 0x00000001);
  snprintf(name, sizeof(name), "%s.%d/GPIO.2", ACPI_GPIO_PATH, index);
  controller_num = ReadFileInt(name);
  if (active_high == -1 || controller_num == -1)
    return -1;                          /* Missing needed info */

  /* Check for chipsets we recognize. */
  snprintf(name, sizeof(name), "%s.%d/GPIO.3", ACPI_GPIO_PATH, index);
  if (!ReadFileString(controller_name, sizeof(controller_name), name))
    return -1;
  if ((0 != strcmp(controller_name, "NM10")) &&
      (0 != strcmp(controller_name, "CougarPoint")) &&
      (0 != strcmp(controller_name, "PantherPoint")))
    return -1;

  /* Modify GPIO number by driver's offset */
  if (!FindGpioChipOffset(&controller_offset))
    return -1;
  controller_offset += controller_num;

  /* Try reading the GPIO value */
  snprintf(name, sizeof(name), "%s/gpio%d/value",
           GPIO_BASE_PATH, controller_offset);
  value = ReadFileInt(name);

  if (value == -1) {
    /* Try exporting the GPIO */
    FILE* f = fopen(GPIO_EXPORT_PATH, "wt");
    if (!f)
      return -1;
    fprintf(f, "%d", controller_offset);
    fclose(f);

    /* Try re-reading the GPIO value */
    value = ReadFileInt(name);
  }

  if (value == -1)
    return -1;

  /* Compare the GPIO value with the active value and return 1 if match. */
  return (value == active_high ? 1 : 0);
}


int VbGetArchPropertyInt(const char* name) {
  int value = -1;

  /* Values from ACPI */
  if (!strcasecmp(name,"recovery_reason")) {
    value = VbGetRecoveryReason();
  } else if (!strcasecmp(name,"fmap_base")) {
    value = ReadFileInt(ACPI_FMAP_PATH);
  }
  /* Switch positions */
  else if (!strcasecmp(name,"devsw_cur")) {
    value = ReadGpio(GPIO_SIGNAL_TYPE_DEV);
  } else if (!strcasecmp(name,"recoverysw_cur")) {
    value = ReadGpio(GPIO_SIGNAL_TYPE_RECOVERY);
  } else if (!strcasecmp(name,"wpsw_cur")) {
    value = ReadGpio(GPIO_SIGNAL_TYPE_WP);
    if (-1 != value && FwidStartsWith("Mario."))
      value = 1 - value;  /* Mario reports this backwards */
  } else if (!strcasecmp(name,"devsw_boot")) {
    value = ReadFileBit(ACPI_CHSW_PATH, CHSW_DEV_BOOT);
  } else if (!strcasecmp(name,"recoverysw_boot")) {
    value = ReadFileBit(ACPI_CHSW_PATH, CHSW_RECOVERY_BOOT);
  } else if (!strcasecmp(name,"recoverysw_ec_boot")) {
    value = ReadFileBit(ACPI_CHSW_PATH, CHSW_RECOVERY_EC_BOOT);
  } else if (!strcasecmp(name,"wpsw_boot")) {
    value = ReadFileBit(ACPI_CHSW_PATH, CHSW_WP_BOOT);
    if (-1 != value && FwidStartsWith("Mario."))
      value = 1 - value;  /* Mario reports this backwards */
  }

  /* Saved memory is at a fixed location for all H2C BIOS.  If the CHSW
   * path exists in sysfs, it's a H2C BIOS. */
  else if (!strcasecmp(name,"savedmem_base")) {
    return (-1 == ReadFileInt(ACPI_CHSW_PATH) ? -1 : 0x00F00000);
  } else if (!strcasecmp(name,"savedmem_size")) {
    return (-1 == ReadFileInt(ACPI_CHSW_PATH) ? -1 : 0x00100000);
  }
  /* NV storage values.  If unable to get from NV storage, fall back to the
   * CMOS reboot field used by older BIOS. */
  else if (!strcasecmp(name,"recovery_request")) {
    value = VbGetNvStorage(VBNV_RECOVERY_REQUEST);
    if (-1 == value)
      value = VbGetCmosRebootField(CMOSRF_RECOVERY);
  } else if (!strcasecmp(name,"dbg_reset")) {
    value = VbGetNvStorage(VBNV_DEBUG_RESET_MODE);
    if (-1 == value)
      value = VbGetCmosRebootField(CMOSRF_DEBUG_RESET);
  } else if (!strcasecmp(name,"fwb_tries")) {
    value = VbGetNvStorage(VBNV_TRY_B_COUNT);
    if (-1 == value)
      value = VbGetCmosRebootField(CMOSRF_TRY_B);
  }
  /* Firmware update tries is now stored in the kernel field.  On
   * older systems where it's not, it was stored in a file in the
   * stateful partition. */
  else if (!strcasecmp(name,"fwupdate_tries")) {
    if (-1 != VbGetNvStorage(VBNV_KERNEL_FIELD))
      return -1;  /* NvStorage supported; fail through arch-specific
                   * implementation to normal implementation. */

    /* Read value from file; missing file means value=0. */
    value = ReadFileInt(NEED_FWUPDATE_PATH);
    if (-1 == value)
      value = 0;
  }

  return value;
}


const char* VbGetArchPropertyString(const char* name, char* dest, int size) {

  if (!strcasecmp(name,"arch")) {
    return StrCopy(dest, "x86", size);
  } else if (!strcasecmp(name,"hwid")) {
    return ReadFileString(dest, size, ACPI_BASE_PATH "/HWID");
  } else if (!strcasecmp(name,"fwid")) {
    return ReadFileString(dest, size, ACPI_BASE_PATH "/FWID");
  } else if (!strcasecmp(name,"ro_fwid")) {
    return ReadFileString(dest, size, ACPI_BASE_PATH "/FRID");
  } else if (!strcasecmp(name,"mainfw_act")) {
    switch(ReadFileInt(ACPI_BINF_PATH ".1")) {
      case 0:
        return StrCopy(dest, "recovery", size);
      case 1:
        return StrCopy(dest, "A", size);
      case 2:
        return StrCopy(dest, "B", size);
      default:
        return NULL;
    }
  } else if (!strcasecmp(name,"mainfw_type")) {
    return VbReadMainFwType(dest, size);
  } else if (!strcasecmp(name,"ecfw_act")) {
    switch(ReadFileInt(ACPI_BINF_PATH ".2")) {
      case 0:
        return StrCopy(dest, "RO", size);
      case 1:
        return StrCopy(dest, "RW", size);
      default:
        return NULL;
    }
  } else if (!strcasecmp(name,"platform_family")) {
    return ReadPlatformFamilyString(dest, size);
  }

  return NULL;
}


int VbSetArchPropertyInt(const char* name, int value) {
  /* NV storage values.  If unable to get from NV storage, fall back to the
   * CMOS reboot field used by older BIOS. */
  if (!strcasecmp(name,"recovery_request")) {
    if (0 == VbSetNvStorage(VBNV_RECOVERY_REQUEST, value))
      return 0;
    return VbSetCmosRebootField(CMOSRF_RECOVERY, value);
  } else if (!strcasecmp(name,"dbg_reset")) {
    if (0 == VbSetNvStorage(VBNV_DEBUG_RESET_MODE, value))
      return 0;
    return  VbSetCmosRebootField(CMOSRF_DEBUG_RESET, value);
  } else if (!strcasecmp(name,"fwb_tries")) {
    if (0 == VbSetNvStorage(VBNV_TRY_B_COUNT, value))
      return 0;
    return VbSetCmosRebootField(CMOSRF_TRY_B, value);
  }
  /* Firmware update tries is now stored in the kernel field.  On
   * older systems where it's not, it was stored in a file in the
   * stateful partition. */
  else if (!strcasecmp(name,"fwupdate_tries")) {
    if (-1 != VbGetNvStorage(VBNV_KERNEL_FIELD))
      return -1;  /* NvStorage supported; fail through arch-specific
                   * implementation to normal implementation */

    if (value) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%d", value);
      return WriteFile(NEED_FWUPDATE_PATH, buf, strlen(buf));
    } else {
      /* No update tries, so remove file if it exists. */
      unlink(NEED_FWUPDATE_PATH);
      return 0;
    }
  }

  return -1;
}


int VbSetArchPropertyString(const char* name, const char* value) {
  /* If there were settable architecture-dependent string properties,
   * they'd be here. */
  return -1;
}
