/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>

#include <platform.h>

#include "build_config.h"

#include "common/color.h"
#include "common/axis.h"
#include "common/maths.h"
#include "common/filter.h"

#include "config/parameter_group.h"
#include "config/parameter_group_ids.h"

#include "drivers/sensor.h"
#include "drivers/accgyro.h"
#include "drivers/compass.h"
#include "drivers/system.h"
#include "drivers/gpio.h"
#include "drivers/timer.h"
#include "drivers/pwm_rx.h"
#include "drivers/serial.h"

#include "io/rc_controls.h"

#include "sensors/sensors.h"
#include "sensors/gyro.h"
#include "sensors/compass.h"
#include "sensors/acceleration.h"
#include "sensors/barometer.h"
#include "sensors/boardalignment.h"
#include "sensors/battery.h"

#include "io/beeper.h"
#include "io/serial.h"
#include "io/gimbal.h"
#include "io/escservo.h"
#include "io/rc_curves.h"
#include "io/ledstrip.h"
#include "io/gps.h"

#include "rx/rx.h"

#include "blackbox/blackbox_io.h"
#include "blackbox/blackbox.h"

#include "telemetry/telemetry.h"

#include "flight/mixer.h"
#include "flight/pid.h"
#include "flight/gtune.h"
#include "flight/imu.h"
#include "flight/failsafe.h"
#include "flight/altitudehold.h"
#include "flight/navigation.h"

#include "config/runtime_config.h"
#include "config/config.h"
#include "config/config_eeprom.h"
#include "config/parameter_group.h"
#include "config/config_streamer.h"

#include "config/config_profile.h"
#include "config/config_master.h"

#ifndef DEFAULT_RX_FEATURE
#define DEFAULT_RX_FEATURE FEATURE_RX_PARALLEL_PWM
#endif

#define BRUSHED_MOTORS_PWM_RATE 16000
#define BRUSHLESS_MOTORS_PWM_RATE 400

static uint32_t activeFeaturesLatch = 0;

static uint8_t currentControlRateProfileIndex = 0;
controlRateConfig_t *currentControlRateProfile;

static const void *pg_registry_tail PG_REGISTRY_TAIL_SECTION;

master_t masterConfig;                 // master config struct with data independent from profiles
static const pgRegistry_t masterRegistry PG_REGISTRY_SECTION = {
    .base = (uint8_t *)&masterConfig,
    .size = sizeof(masterConfig),
    .pgn = PG_MASTER,
    .flags = PGC_SYSTEM
};

STATIC_UNIT_TESTED profile_t profileStorage[MAX_PROFILE_COUNT];
profile_t *currentProfile;

static const pgRegistry_t profileRegistry PG_REGISTRY_SECTION =
{
    .base = (uint8_t *)&profileStorage,
    .ptr = (uint8_t **)&currentProfile,
    .size = sizeof(profileStorage[0]),
    .pgn = PG_PROFILE,
    .format = 0,
    .flags = PGC_PROFILE
};


// FIXME this should probably be defined in a separate file.  Drivers should be aware of parameter groups.

static const pgRegistry_t pwmRxConfigRegistry PG_REGISTRY_SECTION =
{
    .base = (uint8_t *)&pwmRxConfig,
    .size = sizeof(pwmRxConfig),
    .pgn = PG_DRIVER_PWM_RX_CONFIG,
    .format = 0,
    .flags = PGC_SYSTEM
};

void resetPidProfile(pidProfile_t *pidProfile)
{
    pidProfile->pidController = 1;

    pidProfile->P8[PIDROLL] = 40;
    pidProfile->I8[PIDROLL] = 30;
    pidProfile->D8[PIDROLL] = 23;
    pidProfile->P8[PIDPITCH] = 40;
    pidProfile->I8[PIDPITCH] = 30;
    pidProfile->D8[PIDPITCH] = 23;
    pidProfile->P8[PIDYAW] = 85;
    pidProfile->I8[PIDYAW] = 45;
    pidProfile->D8[PIDYAW] = 0;
    pidProfile->P8[PIDALT] = 50;
    pidProfile->I8[PIDALT] = 0;
    pidProfile->D8[PIDALT] = 0;
    pidProfile->P8[PIDPOS] = 15; // POSHOLD_P * 100;
    pidProfile->I8[PIDPOS] = 0; // POSHOLD_I * 100;
    pidProfile->D8[PIDPOS] = 0;
    pidProfile->P8[PIDPOSR] = 34; // POSHOLD_RATE_P * 10;
    pidProfile->I8[PIDPOSR] = 14; // POSHOLD_RATE_I * 100;
    pidProfile->D8[PIDPOSR] = 53; // POSHOLD_RATE_D * 1000;
    pidProfile->P8[PIDNAVR] = 25; // NAV_P * 10;
    pidProfile->I8[PIDNAVR] = 33; // NAV_I * 100;
    pidProfile->D8[PIDNAVR] = 83; // NAV_D * 1000;
    pidProfile->P8[PIDLEVEL] = 20;
    pidProfile->I8[PIDLEVEL] = 10;
    pidProfile->D8[PIDLEVEL] = 100;
    pidProfile->P8[PIDMAG] = 40;
    pidProfile->P8[PIDVEL] = 120;
    pidProfile->I8[PIDVEL] = 45;
    pidProfile->D8[PIDVEL] = 1;

    pidProfile->yaw_p_limit = YAW_P_LIMIT_MAX;
    pidProfile->dterm_cut_hz = 0;
    pidProfile->deltaMethod = 1;

    pidProfile->P_f[FD_ROLL] = 1.4f;     // new PID with preliminary defaults test carefully
    pidProfile->I_f[FD_ROLL] = 0.4f;
    pidProfile->D_f[FD_ROLL] = 0.03f;
    pidProfile->P_f[FD_PITCH] = 1.4f;
    pidProfile->I_f[FD_PITCH] = 0.4f;
    pidProfile->D_f[FD_PITCH] = 0.03f;
    pidProfile->P_f[FD_YAW] = 3.5f;
    pidProfile->I_f[FD_YAW] = 0.4f;
    pidProfile->D_f[FD_YAW] = 0.01f;
    pidProfile->A_level = 5.0f;
    pidProfile->H_level = 3.0f;
    pidProfile->H_sensitivity = 75;
}

#ifdef GTUNE
void resetGTuneConfig(gtuneConfig_t *gtuneConfig)
{
    gtuneConfig->gtune_lolimP[FD_ROLL] = 10;          // [0..200] Lower limit of ROLL P during G tune.
    gtuneConfig->gtune_lolimP[FD_PITCH] = 10;         // [0..200] Lower limit of PITCH P during G tune.
    gtuneConfig->gtune_lolimP[FD_YAW] = 10;           // [0..200] Lower limit of YAW P during G tune.
    gtuneConfig->gtune_hilimP[FD_ROLL] = 100;         // [0..200] Higher limit of ROLL P during G tune. 0 Disables tuning for that axis.
    gtuneConfig->gtune_hilimP[FD_PITCH] = 100;        // [0..200] Higher limit of PITCH P during G tune. 0 Disables tuning for that axis.
    gtuneConfig->gtune_hilimP[FD_YAW] = 100;          // [0..200] Higher limit of YAW P during G tune. 0 Disables tuning for that axis.
    gtuneConfig->gtune_pwr = 0;                    // [0..10] Strength of adjustment
    gtuneConfig->gtune_settle_time = 450;          // [200..1000] Settle time in ms
    gtuneConfig->gtune_average_cycles = 16;        // [8..128] Number of looptime cycles used for gyro average calculation
}
#endif

#ifdef GPS
void resetGpsProfile(gpsProfile_t *gpsProfile)
{
    gpsProfile->gps_wp_radius = 200;
    gpsProfile->gps_lpf = 20;
    gpsProfile->nav_slew_rate = 30;
    gpsProfile->nav_controls_heading = 1;
    gpsProfile->nav_speed_min = 100;
    gpsProfile->nav_speed_max = 300;
    gpsProfile->ap_mode = 40;
}
#endif

#ifdef BARO
void resetBarometerConfig(barometerConfig_t *barometerConfig)
{
    barometerConfig->baro_sample_count = 21;
    barometerConfig->baro_noise_lpf = 0.6f;
    barometerConfig->baro_cf_vel = 0.985f;
    barometerConfig->baro_cf_alt = 0.965f;
}
#endif

void resetEscAndServoConfig(escAndServoConfig_t *escAndServoConfig)
{
    escAndServoConfig->minthrottle = 1150;
    escAndServoConfig->maxthrottle = 1850;
    escAndServoConfig->mincommand = 1000;
    escAndServoConfig->servoCenterPulse = 1500;
}

void resetFlight3DConfig(flight3DConfig_t *flight3DConfig)
{
    flight3DConfig->deadband3d_low = 1406;
    flight3DConfig->deadband3d_high = 1514;
    flight3DConfig->neutral3d = 1460;
    flight3DConfig->deadband3d_throttle = 50;
}

static void resetBatteryConfig(batteryConfig_t *batteryConfig)
{
    batteryConfig->vbatscale = VBAT_SCALE_DEFAULT;
    batteryConfig->vbatresdivval = VBAT_RESDIVVAL_DEFAULT;
    batteryConfig->vbatresdivmultiplier = VBAT_RESDIVMULTIPLIER_DEFAULT;
    batteryConfig->vbatmaxcellvoltage = 43;
    batteryConfig->vbatmincellvoltage = 33;
    batteryConfig->vbatwarningcellvoltage = 35;
    batteryConfig->currentMeterScale = 400; // for Allegro ACS758LCB-100U (40mV/A)
    batteryConfig->currentMeterType = CURRENT_SENSOR_ADC;
}

#ifdef SWAP_SERIAL_PORT_0_AND_1_DEFAULTS
#define FIRST_PORT_INDEX 1
#define SECOND_PORT_INDEX 0
#else
#define FIRST_PORT_INDEX 0
#define SECOND_PORT_INDEX 1
#endif

void resetSerialConfig(serialConfig_t *serialConfig)
{
    uint8_t index;
    memset(serialConfig, 0, sizeof(serialConfig_t));

    for (index = 0; index < SERIAL_PORT_COUNT; index++) {
        serialConfig->portConfigs[index].identifier = serialPortIdentifiers[index];
        serialConfig->portConfigs[index].msp_baudrateIndex = BAUD_115200;
        serialConfig->portConfigs[index].gps_baudrateIndex = BAUD_57600;
        serialConfig->portConfigs[index].telemetry_baudrateIndex = BAUD_AUTO;
        serialConfig->portConfigs[index].blackbox_baudrateIndex = BAUD_115200;
    }

    serialConfig->portConfigs[0].functionMask = FUNCTION_MSP;

#if defined(USE_VCP)
    // This allows MSP connection via USART & VCP so the board can be reconfigured.
    serialConfig->portConfigs[1].functionMask = FUNCTION_MSP;
#endif

    serialConfig->reboot_character = 'R';
}

static void resetControlRateConfig(controlRateConfig_t *controlRateConfig) {
    controlRateConfig->rcRate8 = 90;
    controlRateConfig->rcExpo8 = 65;
    controlRateConfig->thrMid8 = 50;
    controlRateConfig->tpa_breakpoint = 1500;
}

void resetRcControlsConfig(rcControlsConfig_t *rcControlsConfig) {
    rcControlsConfig->deadband = 0;
    rcControlsConfig->yaw_deadband = 0;
    rcControlsConfig->alt_hold_deadband = 40;
    rcControlsConfig->alt_hold_fast_change = 1;
}

static void resetMixerConfig(mixerConfig_t *mixerConfig) {
    mixerConfig->pid_at_min_throttle = 1;
    mixerConfig->airmode_saturation_limit = 50;
    mixerConfig->yaw_motor_direction = 1;
    mixerConfig->yaw_jump_prevention_limit = 200;
#ifdef USE_SERVOS
    mixerConfig->tri_unarmed_servo = 1;
    mixerConfig->servo_lowpass_freq = 400.0f;
#endif
}

void resetRollAndPitchTrims(rollAndPitchTrims_t *rollAndPitchTrims)
{
    rollAndPitchTrims->values.roll = 0;
    rollAndPitchTrims->values.pitch = 0;
}

uint8_t getCurrentProfile(void)
{
    return masterConfig.current_profile_index;
}

static void setProfile(uint8_t profileIndex)
{
    activateProfile(profileIndex);
}

uint8_t getCurrentControlRateProfile(void)
{
    return currentControlRateProfileIndex;
}

controlRateConfig_t *getControlRateConfig(uint8_t profileIndex) {
    return &controlRateProfiles[profileIndex];
}

static void setControlRateProfile(uint8_t profileIndex)
{
    currentControlRateProfileIndex = profileIndex;
    currentControlRateProfile = &controlRateProfiles[profileIndex];
}

uint16_t getCurrentMinthrottle(void)
{
    return escAndServoConfig.minthrottle;
}

// Default settings
STATIC_UNIT_TESTED void resetConf(void)
{
    int i;

    pgResetAll(MAX_PROFILE_COUNT);

    setProfile(0);
    setControlRateProfile(0);

    masterConfig.mixerMode = MIXER_QUADX;
    featureClearAll();
#if defined(CJMCU) || defined(SPARKY) || defined(COLIBRI_RACE) || defined(MOTOLAB) || defined(SPRACINGF3MINI) || defined(LUX_RACE)
    featureSet(FEATURE_RX_PPM);
#endif

//#if defined(SPRACINGF3MINI)
//    featureSet(FEATURE_DISPLAY);
//#endif

#ifdef BOARD_HAS_VOLTAGE_DIVIDER
    // only enable the VBAT feature by default if the board has a voltage divider otherwise
    // the user may see incorrect readings and unexpected issues with pin mappings may occur.
    featureSet(FEATURE_VBAT);
#endif

    featureSet(FEATURE_FAILSAFE);

    // global settings
    masterConfig.dcm_kp = 2500;                // 1.0 * 10000
    gyroConfig.gyro_lpf = 1;                 // supported by all gyro drivers now. In case of ST gyro, will default to 32Hz instead
    gyroConfig.soft_gyro_lpf_hz = 60;        // Software based lpf filter for gyro

    masterConfig.max_angle_inclination = 500;    // 50 degrees
    masterConfig.yaw_control_direction = 1;
    gyroConfig.gyroMovementCalibrationThreshold = 32;

    resetBatteryConfig(&batteryConfig);

#ifdef TELEMETRY
    masterConfig.telemetryConfig.hottAlarmSoundInterval = 5;
#endif

    masterConfig.rxConfig.sbus_inversion = 1;
    masterConfig.rxConfig.midrc = 1500;
    masterConfig.rxConfig.mincheck = 1100;
    masterConfig.rxConfig.maxcheck = 1900;
    masterConfig.rxConfig.rx_min_usec = 885;          // any of first 4 channels below this value will trigger rx loss detection
    masterConfig.rxConfig.rx_max_usec = 2115;         // any of first 4 channels above this value will trigger rx loss detection

    for (i = 0; i < MAX_SUPPORTED_RC_CHANNEL_COUNT; i++) {
        rxFailsafeChannelConfiguration_t *channelFailsafeConfiguration = &masterConfig.rxConfig.failsafe_channel_configurations[i];
        channelFailsafeConfiguration->mode = (i < NON_AUX_CHANNEL_COUNT) ? RX_FAILSAFE_MODE_AUTO : RX_FAILSAFE_MODE_HOLD;
        channelFailsafeConfiguration->step = (i == THROTTLE) ? CHANNEL_VALUE_TO_RXFAIL_STEP(masterConfig.rxConfig.rx_min_usec) : CHANNEL_VALUE_TO_RXFAIL_STEP(masterConfig.rxConfig.midrc);
    }

    masterConfig.rxConfig.rssi_scale = RSSI_SCALE_DEFAULT;

    resetAllRxChannelRangeConfigurations(masterConfig.rxConfig.channelRanges);

    masterConfig.disarm_kill_switch = 1;
    masterConfig.auto_disarm_delay = 5;
    masterConfig.small_angle = 25;

    resetMixerConfig(&masterConfig.mixerConfig);

    masterConfig.airplaneConfig.fixedwing_althold_dir = 1;

    // Motor/ESC/Servo
    resetEscAndServoConfig(&escAndServoConfig);
    resetFlight3DConfig(&masterConfig.flight3DConfig);

#ifdef BRUSHED_MOTORS
    escAndServoConfig.motor_pwm_rate = BRUSHED_MOTORS_PWM_RATE;
#else
    escAndServoConfig.motor_pwm_rate = BRUSHLESS_MOTORS_PWM_RATE;
#endif
    escAndServoConfig.servo_pwm_rate = 50;

#ifdef GPS
    // gps/nav stuff
    masterConfig.gpsConfig.autoConfig = GPS_AUTOCONFIG_ON;
#endif

    resetSerialConfig(&serialConfig);

    masterConfig.looptime = 2000;
    masterConfig.i2c_highspeed = 1;
    masterConfig.gyroSync = 1;
    masterConfig.gyroSyncDenominator = 1;

    resetPidProfile(pidProfile);
#ifdef GTUNE
    resetGTuneConfig(gtuneConfig);
#endif

    resetControlRateConfig(&controlRateProfiles[0]);

    // for (i = 0; i < CHECKBOXITEMS; i++)
    //     cfg.activate[i] = 0;

    resetRollAndPitchTrims(&currentProfile->accelerometerTrims);

    currentProfile->mag_declination = 0;
    currentProfile->acc_cut_hz = 15;
    currentProfile->accz_lpf_cutoff = 5.0f;
    currentProfile->accDeadband.xy = 40;
    currentProfile->accDeadband.z = 40;
    currentProfile->acc_unarmedcal = 1;

#ifdef BARO
    resetBarometerConfig(&currentProfile->barometerConfig);
#endif

    // Radio
    parseRcChannels("AETR1234", &masterConfig.rxConfig);

    resetRcControlsConfig(&currentProfile->rcControlsConfig);

    currentProfile->throttle_correction_value = 0;      // could 10 with althold or 40 for fpv
    currentProfile->throttle_correction_angle = 800;    // could be 80.0 deg with atlhold or 45.0 for fpv

    // Failsafe Variables
    failsafeConfig.failsafe_delay = 10;              // 1sec
    failsafeConfig.failsafe_off_delay = 200;         // 20sec
    failsafeConfig.failsafe_throttle = 1000;         // default throttle off.
    failsafeConfig.failsafe_throttle_low_delay = 100; // default throttle low delay for "just disarm" on failsafe condition

#ifdef USE_SERVOS
    // servos
    for (i = 0; i < MAX_SUPPORTED_SERVOS; i++) {
        currentProfile->servoConf[i].min = DEFAULT_SERVO_MIN;
        currentProfile->servoConf[i].max = DEFAULT_SERVO_MAX;
        currentProfile->servoConf[i].middle = DEFAULT_SERVO_MIDDLE;
        currentProfile->servoConf[i].rate = 100;
        currentProfile->servoConf[i].angleAtMin = DEFAULT_SERVO_MIN_ANGLE;
        currentProfile->servoConf[i].angleAtMax = DEFAULT_SERVO_MAX_ANGLE;
        currentProfile->servoConf[i].forwardFromChannel = CHANNEL_FORWARDING_DISABLED;
    }

    // gimbal
    gimbalConfig->mode = GIMBAL_MODE_NORMAL;
#endif

#ifdef GPS
    resetGpsProfile(&currentProfile->gpsProfile);
#endif

#ifdef LED_STRIP
    applyDefaultColors(masterConfig.colors, CONFIGURABLE_COLOR_COUNT);
    applyDefaultLedStripConfig(masterConfig.ledConfigs);
#endif

#ifdef TRANSPONDER
    static const uint8_t defaultTransponderData[6] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC }; // Note, this is NOT a valid transponder code, it's just for testing production hardware

    memcpy(masterConfig.transponderData, &defaultTransponderData, sizeof(defaultTransponderData));
#endif

#ifdef BLACKBOX

#if defined(ENABLE_BLACKBOX_LOGGING_ON_SPIFLASH_BY_DEFAULT)
    featureSet(FEATURE_BLACKBOX);
    blackboxConfig.device = BLACKBOX_DEVICE_FLASH;
#elif defined(ENABLE_BLACKBOX_LOGGING_ON_SDCARD_BY_DEFAULT)
    featureSet(FEATURE_BLACKBOX);
    blackboxConfig.device = BLACKBOX_DEVICE_SDCARD;
#else
    blackboxConfig.device = BLACKBOX_DEVICE_SERIAL;
#endif

    blackboxConfig.rate_num = 1;
    blackboxConfig.rate_denom = 1;
#endif

    // alternative defaults settings for COLIBRI RACE targets
#if defined(COLIBRI_RACE)
    masterConfig.looptime = 1000;

    pidProfile->pidController = 1;

    masterConfig.rxConfig.rcmap[0] = 1;
    masterConfig.rxConfig.rcmap[1] = 2;
    masterConfig.rxConfig.rcmap[2] = 3;
    masterConfig.rxConfig.rcmap[3] = 0;
    masterConfig.rxConfig.rcmap[4] = 4;
    masterConfig.rxConfig.rcmap[5] = 5;
    masterConfig.rxConfig.rcmap[6] = 6;
    masterConfig.rxConfig.rcmap[7] = 7;

    featureSet(FEATURE_ONESHOT125);
    featureSet(FEATURE_VBAT);
    featureSet(FEATURE_LED_STRIP);
    featureSet(FEATURE_FAILSAFE);
#endif

    // alternative defaults settings for ALIENWIIF1 and ALIENWIIF3 targets
#ifdef ALIENWII32
    featureSet(FEATURE_RX_SERIAL);
    featureSet(FEATURE_MOTOR_STOP);
#ifdef ALIENWIIF3
    serialConfig.portConfigs[2].functionMask = FUNCTION_RX_SERIAL;
    batteryConfig.vbatscale = 20;
#else
    serialConfig.portConfigs[1].functionMask = FUNCTION_RX_SERIAL;
#endif
    masterConfig.rxConfig.serialrx_provider = 1;
    masterConfig.rxConfig.spektrum_sat_bind = 5;
    escAndServoConfig.minthrottle = 1000;
    escAndServoConfig.maxthrottle = 2000;
    escAndServoConfig.motor_pwm_rate = 32000;
    masterConfig.looptime = 2000;
    pidProfile->pidController = 3;
    pidProfile->P8[PIDROLL] = 36;
    pidProfile->P8[PIDPITCH] = 36;
    failsafeConfig.failsafe_delay = 2;
    failsafeConfig.failsafe_off_delay = 0;
    currentControlRateProfile->rcRate8 = 130;
    currentControlRateProfile->rates[ROLL] = 20;
    currentControlRateProfile->rates[PITCH] = 20;
    currentControlRateProfile->rates[YAW] = 100;
    parseRcChannels("TAER1234", &masterConfig.rxConfig);

    //  { 1.0f, -0.414178f,  1.0f, -1.0f },          // REAR_R
    customMotorMixer[0].throttle = 1.0f;
    customMotorMixer[0].roll = -0.414178f;
    customMotorMixer[0].pitch = 1.0f;
    customMotorMixer[0].yaw = -1.0f;

    //  { 1.0f, -0.414178f, -1.0f,  1.0f },          // FRONT_R
    customMotorMixer[1].throttle = 1.0f;
    customMotorMixer[1].roll = -0.414178f;
    customMotorMixer[1].pitch = -1.0f;
    customMotorMixer[1].yaw = 1.0f;

    //  { 1.0f,  0.414178f,  1.0f,  1.0f },          // REAR_L
    customMotorMixer[2].throttle = 1.0f;
    customMotorMixer[2].roll = 0.414178f;
    customMotorMixer[2].pitch = 1.0f;
    customMotorMixer[2].yaw = 1.0f;

    //  { 1.0f,  0.414178f, -1.0f, -1.0f },          // FRONT_L
    customMotorMixer[3].throttle = 1.0f;
    customMotorMixer[3].roll = 0.414178f;
    customMotorMixer[3].pitch = -1.0f;
    customMotorMixer[3].yaw = -1.0f;

    //  { 1.0f, -1.0f, -0.414178f, -1.0f },          // MIDFRONT_R
    customMotorMixer[4].throttle = 1.0f;
    customMotorMixer[4].roll = -1.0f;
    customMotorMixer[4].pitch = -0.414178f;
    customMotorMixer[4].yaw = -1.0f;

    //  { 1.0f,  1.0f, -0.414178f,  1.0f },          // MIDFRONT_L
    customMotorMixer[5].throttle = 1.0f;
    customMotorMixer[5].roll = 1.0f;
    customMotorMixer[5].pitch = -0.414178f;
    customMotorMixer[5].yaw = 1.0f;

    //  { 1.0f, -1.0f,  0.414178f,  1.0f },          // MIDREAR_R
    customMotorMixer[6].throttle = 1.0f;
    customMotorMixer[6].roll = -1.0f;
    customMotorMixer[6].pitch = 0.414178f;
    customMotorMixer[6].yaw = 1.0f;

    //  { 1.0f,  1.0f,  0.414178f, -1.0f },          // MIDREAR_L
    customMotorMixer[7].throttle = 1.0f;
    customMotorMixer[7].roll = 1.0f;
    customMotorMixer[7].pitch = 0.414178f;
    customMotorMixer[7].yaw = -1.0f;
#endif

    // FIXME implement differently

    // copy first profile into remaining profile
    for (i = 1; i < MAX_PROFILE_COUNT; i++) {
        memcpy(&profileStorage[i], &profileStorage[0], sizeof(profile_t));
    }

    // copy first control rate config into remaining profile
    for (i = 1; i < MAX_CONTROL_RATE_PROFILE_COUNT; i++) {
        memcpy(&controlRateProfiles[i], &controlRateProfiles[0], sizeof(controlRateConfig_t));
    }

    for (i = 1; i < MAX_PROFILE_COUNT; i++) {
        profileStorage[i].defaultRateProfileIndex = i % MAX_CONTROL_RATE_PROFILE_COUNT;
    }
}

void activateControlRateConfig(void)
{
    generatePitchRollCurve(currentControlRateProfile);
    generateYawCurve(currentControlRateProfile);
    generateThrottleCurve(currentControlRateProfile, &escAndServoConfig);
}

void activateConfig(void)
{
    static imuRuntimeConfig_t imuRuntimeConfig;

    activateControlRateConfig();

    resetAdjustmentStates();

    useRcControlsConfig(
        currentProfile->modeActivationConditions
    );

#ifdef TELEMETRY
    telemetryUseConfig(&masterConfig.telemetryConfig);
#endif

    pidSetController(pidProfile->pidController);

#ifdef GPS
    gpsUseProfile(&currentProfile->gpsProfile);
    gpsUsePIDs(pidProfile);
#endif

    useFailsafeConfig();
    setAccelerationTrims(&sensorTrims.accZero);

    mixerUseConfigs(
#ifdef USE_SERVOS
        currentProfile->servoConf,
#endif
        &masterConfig.flight3DConfig,
        &masterConfig.mixerConfig,
        &masterConfig.airplaneConfig,
        &masterConfig.rxConfig
    );

    imuRuntimeConfig.dcm_kp = masterConfig.dcm_kp / 10000.0f;
    imuRuntimeConfig.dcm_ki = masterConfig.dcm_ki / 10000.0f;
    imuRuntimeConfig.acc_cut_hz = currentProfile->acc_cut_hz;
    imuRuntimeConfig.acc_unarmedcal = currentProfile->acc_unarmedcal;
    imuRuntimeConfig.small_angle = masterConfig.small_angle;

    imuConfigure(
        &imuRuntimeConfig,
        &currentProfile->accDeadband,
        currentProfile->accz_lpf_cutoff,
        currentProfile->throttle_correction_angle
    );

#if defined(BARO) || defined(SONAR)
    configureAltitudeHold(
        &currentProfile->barometerConfig,
        &currentProfile->rcControlsConfig
    );
#endif

#ifdef BARO
    useBarometerConfig(&currentProfile->barometerConfig);
#endif
}

void validateAndFixConfig(void)
{
    if (!(featureConfigured(FEATURE_RX_PARALLEL_PWM) || featureConfigured(FEATURE_RX_PPM) || featureConfigured(FEATURE_RX_SERIAL) || featureConfigured(FEATURE_RX_MSP))) {
        featureSet(DEFAULT_RX_FEATURE);
    }

    if (featureConfigured(FEATURE_RX_PPM)) {
        featureClear(FEATURE_RX_PARALLEL_PWM | FEATURE_RX_SERIAL | FEATURE_RX_MSP);
    }

    if (featureConfigured(FEATURE_RX_MSP)) {
        featureClear(FEATURE_RX_SERIAL | FEATURE_RX_PARALLEL_PWM | FEATURE_RX_PPM);
    }

    if (featureConfigured(FEATURE_RX_SERIAL)) {
        featureClear(FEATURE_RX_PARALLEL_PWM | FEATURE_RX_MSP | FEATURE_RX_PPM);
    }

    if (featureConfigured(FEATURE_RX_PARALLEL_PWM)) {
        featureClear(FEATURE_RX_SERIAL | FEATURE_RX_MSP | FEATURE_RX_PPM);
    }

#ifdef STM32F10X
    // avoid overloading the CPU on F1 targets when using gyro sync and GPS.
    if (masterConfig.gyroSync && masterConfig.gyroSyncDenominator < 2 && featureConfigured(FEATURE_GPS)) {
        masterConfig.gyroSyncDenominator = 2;
    }
#endif

#if defined(LED_STRIP) && (defined(USE_SOFTSERIAL1) || defined(USE_SOFTSERIAL2))
    if (featureConfigured(FEATURE_SOFTSERIAL) && (
            0
#ifdef USE_SOFTSERIAL1
            || (LED_STRIP_TIMER == SOFTSERIAL_1_TIMER)
#endif
#ifdef USE_SOFTSERIAL2
            || (LED_STRIP_TIMER == SOFTSERIAL_2_TIMER)
#endif
    )) {
        // led strip needs the same timer as softserial
        featureClear(FEATURE_LED_STRIP);
    }
#endif

#if defined(CC3D) && defined(DISPLAY) && defined(USE_UART3)
    if (doesConfigurationUsePort(SERIAL_PORT_UART3) && featureConfigured(FEATURE_DISPLAY)) {
        featureClear(FEATURE_DISPLAY);
    }
#endif

#ifdef STM32F303xC
    // hardware supports serial port inversion, make users life easier for those that want to connect SBus RX's
#ifdef TELEMETRY
    masterConfig.telemetryConfig.telemetry_inversion = 1;
#endif
#endif

    /*
     * The retarded_arm setting is incompatible with pid_at_min_throttle because full roll causes the craft to roll over on the ground.
     * The pid_at_min_throttle implementation ignores yaw on the ground, but doesn't currently ignore roll when retarded_arm is enabled.
     */
    if (masterConfig.retarded_arm && masterConfig.mixerConfig.pid_at_min_throttle) {
        masterConfig.mixerConfig.pid_at_min_throttle = 0;
    }

#if defined(LED_STRIP) && defined(TRANSPONDER) && !defined(UNIT_TEST)
    if ((WS2811_DMA_TC_FLAG == TRANSPONDER_DMA_TC_FLAG) && featureConfigured(FEATURE_TRANSPONDER) && featureConfigured(FEATURE_LED_STRIP)) {
        featureClear(FEATURE_LED_STRIP);
    }
#endif


#if defined(CC3D) && defined(SONAR) && defined(USE_SOFTSERIAL1)
    if (featureConfigured(FEATURE_SONAR) && featureConfigured(FEATURE_SOFTSERIAL)) {
        featureClear(FEATURE_SONAR);
    }
#endif

#if defined(COLIBRI_RACE)
    serialConfig.portConfigs[0].functionMask = FUNCTION_MSP;
    if(featureConfigured(FEATURE_RX_SERIAL)) {
        serialConfig.portConfigs[2].functionMask = FUNCTION_RX_SERIAL;
    }
#endif

    useRxConfig(&masterConfig.rxConfig);

    if (!isSerialConfigValid(&serialConfig)) {
        resetSerialConfig(&serialConfig);
    }
}

void readEEPROM(void)
{
    suspendRxSignal();

    // Sanity check
    // Read flash
    if (!scanEEPROM(true)) {
        failureMode(FAILURE_INVALID_EEPROM_CONTENTS);
    }


    if (masterConfig.current_profile_index > MAX_PROFILE_COUNT - 1) // sanity check
        masterConfig.current_profile_index = 0;

    setProfile(masterConfig.current_profile_index);

    if (currentProfile->defaultRateProfileIndex > MAX_CONTROL_RATE_PROFILE_COUNT - 1) // sanity check
        currentProfile->defaultRateProfileIndex = 0;

    setControlRateProfile(currentProfile->defaultRateProfileIndex);

    validateAndFixConfig();
    activateConfig();

    resumeRxSignal();
}

void readEEPROMAndNotify(void)
{
    // re-read written data
    readEEPROM();
    beeperConfirmationBeeps(1);
}

void writeEEPROM(void)
{
    suspendRxSignal();

    writeConfigToEEPROM();

    resumeRxSignal();
}

void ensureEEPROMContainsValidData(void)
{
    if (isEEPROMContentValid()) {
        return;
    }

    resetEEPROM();
}

void resetEEPROM(void)
{
    resetConf();
    writeEEPROM();
}

void saveConfigAndNotify(void)
{
    writeEEPROM();
    readEEPROMAndNotify();
}

void changeProfile(uint8_t profileIndex)
{
    masterConfig.current_profile_index = profileIndex;
    writeEEPROM();
    readEEPROM();
}

void changeControlRateProfile(uint8_t profileIndex)
{
    if (profileIndex > MAX_CONTROL_RATE_PROFILE_COUNT) {
        profileIndex = MAX_CONTROL_RATE_PROFILE_COUNT - 1;
    }
    setControlRateProfile(profileIndex);
    activateControlRateConfig();
}

void handleOneshotFeatureChangeOnRestart(void)
{
    // Shutdown PWM on all motors prior to soft restart
    StopPwmAllMotors();
    delay(50);
    // Apply additional delay when OneShot125 feature changed from on to off state
    if (feature(FEATURE_ONESHOT125) && !featureConfigured(FEATURE_ONESHOT125)) {
        delay(ONESHOT_FEATURE_CHANGED_DELAY_ON_BOOT_MS);
    }
}

void latchActiveFeatures()
{
    activeFeaturesLatch = masterConfig.enabledFeatures;
}

bool featureConfigured(uint32_t mask)
{
    return masterConfig.enabledFeatures & mask;
}

bool feature(uint32_t mask)
{
    return activeFeaturesLatch & mask;
}

void featureSet(uint32_t mask)
{
    masterConfig.enabledFeatures |= mask;
}

void featureClear(uint32_t mask)
{
    masterConfig.enabledFeatures &= ~(mask);
}

void featureClearAll()
{
    masterConfig.enabledFeatures = 0;
}

uint32_t featureMask(void)
{
    return masterConfig.enabledFeatures;
}
