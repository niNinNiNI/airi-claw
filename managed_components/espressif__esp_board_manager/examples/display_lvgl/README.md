# Display LVGL Example

- [中文版](./README_CN.md)

## Example Brief

This example demonstrates how to use the `esp_board_manager` with LVGL on supported development boards. It initializes the board manager, sets up the LVGL adapter, adds the LCD display and touch input (if available), and runs the LVGL test UI.

## Example Set Up

### IDF Default Branch

This example supports IDF release/v5.5 (>=5.5.2) and IDF release/v5.4 (>=5.4.3) branches.

### Build and Flash

Before compiling this example, ensure that the ESP-IDF environment is properly set up. If not, run the following script in the root directory of ESP-IDF to set up the build environment. For detailed steps on configuring and using ESP-IDF, please refer to the [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/index.html):

```shell
./install.sh
. ./export.sh
```

Here are the summarized compilation steps:

- Navigate to the test project directory for driving display with LVGL:

```shell
cd $YOUR_GMF_PATH/packages/esp_board_manager/examples/display_lvgl
```

- Configure the `esp_board_manager` path to activate the environment (only needs to be executed once under the current terminal):

```shell
# Ubuntu and Mac:
export IDF_EXTRA_ACTIONS_PATH=$YOUR_GMF_PATH/packages/esp_board_manager

# Windows PowerShell:
$env:IDF_EXTRA_ACTIONS_PATH = "$YOUR_GMF_PATH/packages/esp_board_manager"

# Windows Command Prompt (CMD):
set IDF_EXTRA_ACTIONS_PATH=$YOUR_GMF_PATH/packages/esp_board_manager
```

- Select the development board to use:

```shell
idf.py gen-bmgr-config -b esp32_s3_box_2
```

- You can also run the following command to see a list of supported development boards:

```shell
idf.py gen-bmgr-config -l
```

- Compile the example code:

```shell
idf.py build
```

- Flash the program and run the monitor tool to view serial output (replace PORT with your port name):

```shell
idf.py -p PORT flash monitor
```

- To exit the debugging interface, use `Ctrl-]`.

## How to Use the Example

### Functionality and Usage

- After the example starts, it will automatically initialize the display and run the LVGL test UI. The output will be as follows:

```c
I (829) BMGR_DISPLAY_LVGL: Starting LVGL Example
...
I (889) DEV_DISPLAY_LCD: Initializing LCD display: display_lcd, chip: st7789, sub_type: i80
I (899) DEV_DISPLAY_LCD_SUB_I80: Initializing I80 LCD display: display_lcd, chip: st7789
I (1029) DEV_DISPLAY_LCD: Successfully initialized LCD display: display_lcd (sub_type: i80), panel: 0x3fce9f40, io: 0x3c0b09e8
I (1029) BOARD_MANAGER: Device display_lcd initialized
...
I (1049) BMGR_DISPLAY_LVGL: Initializing LVGL adapter...
I (1059) LVGL: Starting LVGL task
I (1059) BOARD_DEVICE: Device handle display_lcd found, Handle: 0x3fce9b14 TO: 0x3fce9b14
I (1069) BOARD_DEVICE: Device display_lcd config found: 0x3c0a4ba0 (size: 184)
I (1079) BMGR_DISPLAY_LVGL: Running LVGL Test UI
I (1079) lvgl_test_ui: Starting LCD LVGL test sequence
I (1089) lvgl_test_ui: Testing Magenta screen
I (1109) lvgl_test_ui: cleanup_ui_elements done
I (4219) lvgl_test_ui: cleanup_ui_elements done
I (4719) lvgl_test_ui: Testing Cyan screen
I (4719) lvgl_test_ui: cleanup_ui_elements done
I (7819) lvgl_test_ui: cleanup_ui_elements done
I (8319) lvgl_test_ui: Testing Blue screen
I (8319) lvgl_test_ui: cleanup_ui_elements done
I (11419) lvgl_test_ui: cleanup_ui_elements done
I (11919) lvgl_test_ui: Testing White screen
I (11919) lvgl_test_ui: cleanup_ui_elements done
I (15019) lvgl_test_ui: cleanup_ui_elements done
I (15519) lvgl_test_ui: show_test_results start
I (15519) lvgl_test_ui: show_test_results UI update done
I (18519) lvgl_test_ui: show_test_results done
I (18519) lvgl_test_ui: Test sequence completed. Result: FAIL.
I (18519) BMGR_DISPLAY_LVGL: Example Finished. Exiting app_main...
...
I (18629) main_task: Returned from app_main()
```

## Troubleshooting

1. If you encounter the following error message, please run `echo $IDF_EXTRA_ACTIONS_PATH` to check if the `esp_board_manager` path is configured correctly:

```c
Usage: idf.py gen-bmgr-config [OPTIONS]
Try 'idf.py gen-bmgr-config --help' for help.

Error: No such option: -b
```

2. If you need to use a custom development board, please refer to the instructions on **custom boards** in [README.md](../../../README.md).
