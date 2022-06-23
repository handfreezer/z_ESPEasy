#include "_Plugin_Helper.h"
#ifdef USES_P111

// #######################################################################################################
// ################################ Plugin-111: RC522 SPI RFID reader ####################################
// #######################################################################################################

// Changelog:
// 2022-06-23, tonhuisman: Reformat source (uncrustify), optimize somewhat for size
//                         Replace delay() call in reset by handling via plugin_fifty_per_second
// 2021-03-13, tonhuisman: Disabled tag removal detection, as it seems impossible to achieve with the MFRC522.
//                         Other takers to try and solve this challenge are welcome.
//                         If this feature is desired, use a PN532 RFID detector, that does support removal detection properly and easily.
//                         Set TimerOption to false as nothing is processed during PLUGIN_READ stage.
// 2021-02-10, tonhuisman: Add tag removal detection, can be combined with time-out
// 2021-02-07, tonhuisman: Rework to adhere to current plugin requirements, make pin settings user-selectable
//                         Add options for tag removal time-out, as implemented before in P008 (Wiegand RFID) and P017 (PN532 RFID)
//                         Implement PluginStruct to enable multiple instances
// 2021-02-07, twinbee77: Adjustments to P129 from PluginPlayground

# define PLUGIN_111
# define PLUGIN_ID_111         111
# define PLUGIN_NAME_111       "RFID - RC522 [TESTING]"
# define PLUGIN_VALUENAME1_111 "Tag"

# include "src/PluginStructs/P111_data_struct.h"

# define P111_NO_KEY           0xFFFFFFFF

// #define P111_USE_REMOVAL      // Enable (real) Tag Removal detection options (but that won't work with MFRC522 reader)

boolean Plugin_111(uint8_t function, struct EventStruct *event, String& string)
{
  boolean success = false;

  switch (function)
  {
    case PLUGIN_DEVICE_ADD:
    {
      Device[++deviceCount].Number           = PLUGIN_ID_111;
      Device[deviceCount].Type               = DEVICE_TYPE_SPI2;
      Device[deviceCount].VType              = Sensor_VType::SENSOR_TYPE_LONG;
      Device[deviceCount].Ports              = 0;
      Device[deviceCount].PullUpOption       = false;
      Device[deviceCount].InverseLogicOption = false;
      Device[deviceCount].ValueCount         = 1;
      Device[deviceCount].SendDataOption     = true;
      Device[deviceCount].TimerOption        = false;
      Device[deviceCount].GlobalSyncOption   = true;
      break;
    }

    case PLUGIN_GET_DEVICENAME:
    {
      string = F(PLUGIN_NAME_111);
      break;
    }

    case PLUGIN_GET_DEVICEVALUENAMES:
    {
      strcpy_P(ExtraTaskSettings.TaskDeviceValueNames[0], PSTR(PLUGIN_VALUENAME1_111));
      break;
    }

    case PLUGIN_GET_DEVICEGPIONAMES:                                   // define 'GPIO 1st' name in webserver
    {
      event->String1 = formatGpioName_output(F("CS PIN"));             // P111_CS_PIN
      event->String2 = formatGpioName_output(F("RST PIN (optional)")); // P111_RST_PIN
      break;
    }

    case PLUGIN_SET_DEFAULTS:
    {
      P111_REMOVALTIMEOUT = 500; // Default 500 msec reset delay
      break;
    }

    case PLUGIN_WEBFORM_LOAD:
    {
      addFormSubHeader(F("Options"));

      {
        # ifdef P111_USE_REMOVAL
        #  define P111_removaltypes 3
        # else // ifdef P111_USE_REMOVAL
        #  define P111_removaltypes 2
        # endif // ifdef P111_USE_REMOVAL
        const __FlashStringHelper *removaltype[P111_removaltypes] = {
          F("None"),
          F("Autoremove after Time-out"),
          # ifdef P111_USE_REMOVAL
          F("Tag removal detection + Time-out")
          # endif // ifdef P111_USE_REMOVAL
        };
        const int    removalopts[P111_removaltypes] = { // A-typical order for logical order and backward compatibility
          1, 0,
          # ifdef P111_USE_REMOVAL
          2
          # endif // P111_USE_REMOVAL
        };
        addFormSelector(F("Tag removal mode"), F("autotagremoval"), P111_removaltypes, removaltype, removalopts, P111_TAG_AUTOREMOVAL);
      }

      addFormNumericBox(F("Tag removal Time-out"), F("removaltimeout"), P111_REMOVALTIMEOUT, 0, 60000);         // 0 to 60 seconds
      addUnit(F("mSec. (0..60000)"));

      addFormNumericBox(F("Value to set on Tag removal"), F("removalvalue"), P111_REMOVALVALUE, 0, 2147483647); // Max allowed is int =
                                                                                                                // 0x7FFFFFFF ...

      addFormCheckBox(F("Event on Tag removal"), F("sendreset"), P111_SENDRESET == 1);

      success = true;
      break;
    }

    case PLUGIN_WEBFORM_SAVE:
    {
      P111_TAG_AUTOREMOVAL = getFormItemInt(F("autotagremoval"));
      P111_SENDRESET       = isFormItemChecked(F("sendreset")) ? 1 : 0;
      P111_REMOVALVALUE    = getFormItemInt(F("removalvalue"));
      P111_REMOVALTIMEOUT  = getFormItemInt(F("removaltimeout"));

      success = true;
      break;
    }

    case PLUGIN_INIT:
    {
      initPluginTaskData(event->TaskIndex, new (std::nothrow) P111_data_struct(P111_CS_PIN, P111_RST_PIN));
      P111_data_struct *P111_data = static_cast<P111_data_struct *>(getPluginTaskData(event->TaskIndex));

      if (nullptr != P111_data) {
        P111_data->init();

        success = true;
      }

      break;
    }

    case PLUGIN_TIMER_IN:
    {
      // Reset card id on timeout
      if (P111_TAG_AUTOREMOVAL == 0
          # ifdef P111_USE_REMOVAL
          || P111_TAG_AUTOREMOVAL == 2
          # endif // ifdef P111_USE_REMOVAL
          ) {
        UserVar.setSensorTypeLong(event->TaskIndex, P111_REMOVALVALUE);
        addLog(LOG_LEVEL_INFO, F("MFRC522: Removed Tag"));

        if (P111_SENDRESET == 1) {
          sendData(event);
        }
        success = true;
      }
      break;
    }

    case PLUGIN_TEN_PER_SECOND:
    {
      P111_data_struct *P111_data = static_cast<P111_data_struct *>(getPluginTaskData(event->TaskIndex));

      if (nullptr == P111_data) {
        return success;
      }

      P111_data->counter++;          // This variable replaces a static variable in the original implementation

      if (P111_data->counter == 3) { // Only every 3rd 0.1 second we do a read
        P111_data->counter = 0;

        uint32_t key        = P111_NO_KEY;
        bool     removedTag = false;
        const uint8_t error = P111_data->readCardStatus(&key, &removedTag);

        if (error == P111_NO_ERROR) {
          const uint32_t old_key = UserVar.getSensorTypeLong(event->TaskIndex);
          bool new_key           = false;

          # ifdef P111_USE_REMOVAL

          if (removedTag && (P111_TAG_AUTOREMOVAL == 2)) { // removal detected and enabled
            key = P111_REMOVALVALUE;
          }
          # endif // P111_USE_REMOVAL

          if ((old_key != key) && (key != P111_NO_KEY)) {
            UserVar.setSensorTypeLong(event->TaskIndex, key);
            new_key = true;
          }

          if (loglevelActiveFor(LOG_LEVEL_INFO) && (key != P111_NO_KEY)) {
            String log = F("MFRC522: ");

            if (new_key) {
              log += F("New Tag: ");
            } else {
              log += F("Old Tag: ");
            }
            log += key;

            if (!removedTag) {
              log += F(" card: ");
              log += P111_data->getCardName();
            }
            addLogMove(LOG_LEVEL_INFO, log);
          }

          if (new_key && !removedTag) { // Removal event sent from PLUGIN_TIMER_IN, if any
            sendData(event);
          }
          Scheduler.setPluginTaskTimer(P111_REMOVALTIMEOUT, event->TaskIndex, event->Par1);
          success = true;
        }
      }
      break;
    }

    case PLUGIN_FIFTY_PER_SECOND: // Handle delays
    {
      P111_data_struct *P111_data = static_cast<P111_data_struct *>(getPluginTaskData(event->TaskIndex));

      if (nullptr != P111_data) {
        success = P111_data->plugin_fifty_per_second();
      }
      break;
    }
  }
  return success;
}

#endif // USES_P111
