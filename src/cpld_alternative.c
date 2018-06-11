#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>
#include "defs.h"
#include "cpld.h"
#include "osd.h"
#include "logging.h"
#include "rpi-gpio.h"

// The number of frames to compute differences over
#define NUM_CAL_FRAMES 10

typedef struct {
   int sp_base[NUM_CHANNELS];
   int sp_offset[NUM_OFFSETS];
   int half_px_delay;
} config_t;

// Current calibration state for mode 0..6
static config_t default_config;

// Current calibration state for mode 7
static config_t mode7_config;

// Current configuration
static config_t *config;
static int mode7;

// OSD message buffer
static char message[80];

// An initial value for the metric, to prevent warnings
static int unknown_metric[] = {0, 0, 0};

// =============================================================
// Param definitions for OSD
// =============================================================

enum {
   RED_DELAY,
   GREEN_DELAY,
   BLUE_DELAY,
   A_OFFSET,
   B_OFFSET,
   C_OFFSET,
   D_OFFSET,
   E_OFFSET,
   F_OFFSET,
   HALF
};

static param_t default_params[] = {
   { "Red Delay",   0, 5 },
   { "Green Delay", 0, 5 },
   { "Blue Delay",  0, 5 },
   { "A offset",   -1, 1 },
   { "B offset",   -1, 1 },
   { "C offset",   -1, 1 },
   { "D offset",   -1, 1 },
   { "E offset",   -1, 1 },
   { "F offset",   -1, 1 },
   { "Half",        0, 1 },
   { NULL,          0, 0 },
};

static param_t mode7_params[] = {
   { "Red Delay",   0, 7 },
   { "Green Delay", 0, 7 },
   { "Blue Delay",  0, 7 },
   { "A offset",   -1, 1 },
   { "B offset",   -1, 1 },
   { "C offset",   -1, 1 },
   { "D offset",   -1, 1 },
   { "E offset",   -1, 1 },
   { "F offset",   -1, 1 },
   { "Half",        0, 1 },
   { NULL,          0, 0 },
};

// =============================================================
// Private methods
// =============================================================

static void write_config(config_t *config) {
   int sp = 0;
   for (int i = 0; i < NUM_CHANNELS; i++) {
      sp |= (config->sp_base[i] & 7) << (i * 3);
   }
   if (config->half_px_delay) {
      sp |= (1 << 21);
   }
   for (int i = 0; i < NUM_OFFSETS; i++) {
      switch (config->sp_offset[i]) {
      case -1:
         sp |= 3 << (i * 2 + 9);
         break;
      case 1:
         sp |= 1 << (i * 2 + 9);
         break;
      }
   }
   for (int i = 0; i < 22; i++) {
      RPI_SetGpioValue(SP_DATA_PIN, sp & 1);
      for (int j = 0; j < 1000; j++);
      RPI_SetGpioValue(SP_CLKEN_PIN, 1);
      for (int j = 0; j < 100; j++);
      RPI_SetGpioValue(SP_CLK_PIN, 0);
      RPI_SetGpioValue(SP_CLK_PIN, 1);
      for (int j = 0; j < 100; j++);
      RPI_SetGpioValue(SP_CLKEN_PIN, 0);
      for (int j = 0; j < 1000; j++);
      sp >>= 1;
   }
   RPI_SetGpioValue(SP_DATA_PIN, 0);
}

static void osd_sp(config_t *config, int *metric) {
   sprintf(message, " Delays: R=%d G=%d B=%d  Half: %d",
           config->sp_base[CHAN_RED], config->sp_base[CHAN_GREEN], config->sp_base[CHAN_BLUE], config->half_px_delay);
   osd_set(1, 0, message);
   sprintf(message, "Offsets: %2d %2d %2d %2d %2d %2d",
           config->sp_offset[0], config->sp_offset[1], config->sp_offset[2],
           config->sp_offset[3], config->sp_offset[4], config->sp_offset[5]);
   osd_set(2, 0, message);
   sprintf(message, "Metrics: R=%d G=%d B=%d",
           metric[CHAN_RED], metric[CHAN_GREEN],  metric[CHAN_BLUE]);
   osd_set(3, 0, message);
}

static void log_sp(config_t *config) {
   log_info("sp_base = %d %d %d; sp_offset = %d %d %d %d %d %d; half = %d",
            config->sp_base[CHAN_RED], config->sp_base[CHAN_GREEN], config->sp_base[CHAN_BLUE],
            config->sp_offset[0], config->sp_offset[1], config->sp_offset[2],
            config->sp_offset[3], config->sp_offset[4], config->sp_offset[5],
            config->half_px_delay);
}

// =============================================================
// Public methods
// =============================================================

static void cpld_init() {
   for (int i = 0; i < NUM_CHANNELS; i++) {
      default_config.sp_base[i] = 0;
      mode7_config.sp_base[i] = 0;
   }
   for (int i = 0; i < NUM_OFFSETS; i++) {
      default_config.sp_offset[i] = 0;
      mode7_config.sp_offset[i] = 0;
   }
   default_config.half_px_delay = 0;
   mode7_config.half_px_delay = 0;
   config = &default_config;
}

static void cpld_calibrate(int elk, int chars_per_line) {
   int min_i[NUM_CHANNELS];
   int min_metric[NUM_CHANNELS];
   int *metric = unknown_metric;

   if (mode7) {
      log_info("Calibrating mode 7");
   } else {
      log_info("Calibrating modes 0..6");
   }

   for (int i = 0; i < NUM_CHANNELS; i++) {
      min_metric[i] = INT_MAX;
      min_i[i] = 0;
   }

   for (int i = 0; i < NUM_OFFSETS; i++) {
      config->sp_offset[i] = 0;
   }
   config->half_px_delay = 0;

   for (int i = 0; i <= (mode7 ? 7 : 5); i++) {
      for (int j = 0; j < NUM_CHANNELS; j++) {
         config->sp_base[j] = i;
      }
      write_config(config);
      metric = diff_N_frames(i, NUM_CAL_FRAMES, mode7, elk, chars_per_line);
      osd_sp(config, metric);
      log_info("offset = %d: metric = %d %d %d", i, metric[CHAN_RED], metric[CHAN_GREEN], metric[CHAN_BLUE]);
      for (int j = 0; j < NUM_CHANNELS; j++) {
         if (metric[j] < min_metric[j]) {
            min_metric[j] = metric[j];
            min_i[j] = i;
         }
      }
   }
   if (mode7) {
      // If the min metric is at the limit, make use of the half pixel delay
      int toggle = 0;
      if (min_i[CHAN_RED] <= 1 || min_i[CHAN_GREEN] <= 1 || min_i[CHAN_BLUE] <= 1) {
         if (min_i[CHAN_RED] <= 2 && min_i[CHAN_GREEN] <= 2 && min_i[CHAN_BLUE] <= 2) {
            toggle = 1;
         }
      }
      if (min_i[CHAN_RED] >= 6 || min_i[CHAN_GREEN] >= 6 || min_i[CHAN_BLUE] >= 6) {
         if (min_i[CHAN_RED] >= 5 && min_i[CHAN_GREEN] >= 5 && min_i[CHAN_BLUE] >= 5) {
            toggle = 1;
         }
      }
      if (toggle) {
         log_info("Enabling half pixel delay");
         config->half_px_delay = 1;
         min_i[CHAN_RED] ^= 4;
         min_i[CHAN_GREEN] ^= 4;
         min_i[CHAN_BLUE] ^= 4;
      }
   }

   for (int j = 0; j < NUM_CHANNELS; j++) {
      config->sp_base[j] = min_i[j];
   }

   // If the metric is non zero, there is scope for further optimiation in mode 7
   int ref = min_metric[CHAN_RED] + min_metric[CHAN_GREEN] + min_metric[CHAN_BLUE];
   if (mode7 && ref > 0) {
      log_info("Optimizing calibration");
      log_sp(config);
      log_debug("ref = %d", ref);
      for (int i = 0; i < NUM_OFFSETS; i++) {
         int left = INT_MAX;
         int right = INT_MAX;
         if (config->sp_base[CHAN_RED] > 0 && config->sp_base[CHAN_GREEN] > 0 && config->sp_base[CHAN_BLUE] > 0) {
            config->sp_offset[i] = -1;
            write_config(config);
            metric = diff_N_frames(i, NUM_CAL_FRAMES, mode7, elk, chars_per_line);
            osd_sp(config, metric);
            left = metric[CHAN_RED] + metric[CHAN_GREEN] + metric[CHAN_BLUE];
         }
         if (config->sp_base[CHAN_RED] < 7 && config->sp_base[CHAN_GREEN] < 7 && config->sp_base[CHAN_BLUE] < 7) {
            config->sp_offset[i] = 1;
            write_config(config);
            metric = diff_N_frames(i, NUM_CAL_FRAMES, mode7, elk, chars_per_line);
            osd_sp(config, metric);
            right = metric[CHAN_RED] + metric[CHAN_GREEN] + metric[CHAN_BLUE];
         }
         if (left < right && left < ref) {
            config->sp_offset[i] = -1;
            ref = left;
            log_info("nudged %d left, metric = %d", i, ref);
         } else if (right < left && right < ref) {
            config->sp_offset[i] = 1;
            ref = right;
            log_info("nudged %d right, metric = %d", i, ref);
         } else {
            config->sp_offset[i] = 0;
         }
      }
   }
   log_info("Calibration complete");
   log_sp(config);
   write_config(config);
   osd_sp(config, metric);
}

static void cpld_set_mode(int mode) {
   mode7 = mode;
   config = mode ? &mode7_config : &default_config;
   write_config(config);
}

static param_t *cpld_get_params() {
   if (mode7) {
      return mode7_params;
   } else {
      return default_params;
   }
}

static int cpld_get_value(int num) {
   switch (num) {
   case RED_DELAY:
      return config->sp_base[CHAN_RED];
   case GREEN_DELAY:
      return config->sp_base[CHAN_GREEN];
   case BLUE_DELAY:
      return config->sp_base[CHAN_BLUE];
   case A_OFFSET:
      return config->sp_offset[0];
   case B_OFFSET:
      return config->sp_offset[1];
   case C_OFFSET:
      return config->sp_offset[2];
   case D_OFFSET:
      return config->sp_offset[3];
   case E_OFFSET:
      return config->sp_offset[4];
   case F_OFFSET:
      return config->sp_offset[5];
   case HALF:
      return config->half_px_delay;
   }
   return 0;
}

static void cpld_set_value(int num, int value) {
   switch (num) {
   case RED_DELAY:
      config->sp_base[CHAN_RED] = value;
      break;
   case GREEN_DELAY:
      config->sp_base[CHAN_GREEN] = value;
      break;
   case BLUE_DELAY:
      config->sp_base[CHAN_BLUE] = value;
      break;
   case A_OFFSET:
      config->sp_offset[0] = value;
      break;
   case B_OFFSET:
      config->sp_offset[1] = value;
      break;
   case C_OFFSET:
      config->sp_offset[2] = value;
      break;
   case D_OFFSET:
      config->sp_offset[3] = value;
      break;
   case E_OFFSET:
      config->sp_offset[4] = value;
      break;
   case F_OFFSET:
      config->sp_offset[5] = value;
      break;
   case HALF:
      config->half_px_delay = value;
      break;
   }
   write_config(config);
}

cpld_t cpld_alternative = {
   .name = "Alternative",
   .init = cpld_init,
   .calibrate = cpld_calibrate,
   .set_mode = cpld_set_mode,
   .get_params = cpld_get_params,
   .get_value = cpld_get_value,
   .set_value = cpld_set_value
};
