/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>

#include "bootloader.h"
#include "commands.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"

static const struct option OPTIONS[] = {
  { "send_intent", required_argument, NULL, 's' },
  { "update_package", required_argument, NULL, 'u' },
  { "wipe_data", no_argument, NULL, 'w' },
  { "wipe_cache", no_argument, NULL, 'c' },
};

static const char *COMMAND_FILE = "CACHE:recovery/command";
static const char *INTENT_FILE = "CACHE:recovery/intent";
static const char *LOG_FILE = "CACHE:recovery/log";
static const char *SDCARD_PACKAGE_FILE = "SDCARD:update.zip";
static const char *SDCARD_PATH = "SDCARD:";
#define SDCARD_PATH_LENGTH 7
static const char *TEMPORARY_LOG_FILE = "/tmp/recovery.log";

/*
 * The recovery tool communicates with the main system through /cache files.
 *   /cache/recovery/command - INPUT - command line for tool, one arg per line
 *   /cache/recovery/log - OUTPUT - combined log file from recovery run(s)
 *   /cache/recovery/intent - OUTPUT - intent that was passed in
 *
 * The arguments which may be supplied in the recovery.command file:
 *   --send_intent=anystring - write the text out to recovery.intent
 *   --update_package=root:path - verify install an OTA package file
 *   --wipe_data - erase user data (and cache), then reboot
 *   --wipe_cache - wipe cache (but not user data), then reboot
 *
 * After completing, we remove /cache/recovery/command and reboot.
 * Arguments may also be supplied in the bootloader control block (BCB).
 * These important scenarios must be safely restartable at any point:
 *
 * FACTORY RESET
 * 1. user selects "factory reset"
 * 2. main system writes "--wipe_data" to /cache/recovery/command
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--wipe_data"
 *    -- after this, rebooting will restart the erase --
 * 5. erase_root() reformats /data
 * 6. erase_root() reformats /cache
 * 7. finish_recovery() erases BCB
 *    -- after this, rebooting will restart the main system --
 * 8. main() calls reboot() to boot main system
 *
 * OTA INSTALL
 * 1. main system downloads OTA package to /cache/some-filename.zip
 * 2. main system writes "--update_package=CACHE:some-filename.zip"
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--update_package=..."
 *    -- after this, rebooting will attempt to reinstall the update --
 * 5. install_package() attempts to install the update
 *    NOTE: the package install must itself be restartable from any point
 * 6. finish_recovery() erases BCB
 *    -- after this, rebooting will (try to) restart the main system --
 * 7. ** if install failed **
 *    7a. prompt_and_wait() shows an error icon and waits for the user
 *    7b; the user reboots (pulling the battery, etc) into the main system
 * 8. main() calls maybe_install_firmware_update()
 *    ** if the update contained radio/hboot firmware **:
 *    8a. m_i_f_u() writes BCB with "boot-recovery" and "--wipe_cache"
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8b. m_i_f_u() writes firmware image into raw cache partition
 *    8c. m_i_f_u() writes BCB with "update-radio/hboot" and "--wipe_cache"
 *        -- after this, rebooting will attempt to reinstall firmware --
 *    8d. bootloader tries to flash firmware
 *    8e. bootloader writes BCB with "boot-recovery" (keeping "--wipe_cache")
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8f. erase_root() reformats /cache
 *    8g. finish_recovery() erases BCB
 *        -- after this, rebooting will (try to) restart the main system --
 * 9. main() calls reboot() to boot main system
 */

static const int MAX_ARG_LENGTH = 4096;
static const int MAX_ARGS = 100;

static int do_reboot = 1;

// open a file given in root:path format, mounting partitions as necessary
static FILE*
fopen_root_path(const char *root_path, const char *mode) {
    if (ensure_root_path_mounted(root_path) != 0) {
        LOGE("Can't mount %s\n", root_path);
        return NULL;
    }

    char path[PATH_MAX] = "";
    if (translate_root_path(root_path, path, sizeof(path)) == NULL) {
        LOGE("Bad path %s\n", root_path);
        return NULL;
    }

    // When writing, try to create the containing directory, if necessary.
    // Use generous permissions, the system (init.rc) will reset them.
    if (strchr("wa", mode[0])) dirCreateHierarchy(path, 0777, NULL, 1);

    FILE *fp = fopen(path, mode);
    return fp;
}

// close a file, log an error if the error indicator is set
static void
check_and_fclose(FILE *fp, const char *name) {
    fflush(fp);
    if (ferror(fp)) LOGE("Error in %s\n(%s)\n", name, strerror(errno));
    fclose(fp);
}

// command line args come from, in decreasing precedence:
//   - the actual command line
//   - the bootloader control block (one per line, after "recovery")
//   - the contents of COMMAND_FILE (one per line)
static void
get_args(int *argc, char ***argv) {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    get_bootloader_message(&boot);  // this may fail, leaving a zeroed structure

    if (boot.command[0] != 0 && boot.command[0] != 255) {
        LOGI("Boot command: %.*s\n", sizeof(boot.command), boot.command);
    }

    if (boot.status[0] != 0 && boot.status[0] != 255) {
        LOGI("Boot status: %.*s\n", sizeof(boot.status), boot.status);
    }

    // --- if arguments weren't supplied, look in the bootloader control block
    if (*argc <= 1) {
        boot.recovery[sizeof(boot.recovery) - 1] = '\0';  // Ensure termination
        const char *arg = strtok(boot.recovery, "\n");
        if (arg != NULL && !strcmp(arg, "recovery")) {
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = strdup(arg);
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if ((arg = strtok(NULL, "\n")) == NULL) break;
                (*argv)[*argc] = strdup(arg);
            }
            LOGI("Got arguments from boot message\n");
        } else if (boot.recovery[0] != 0 && boot.recovery[0] != 255) {
            LOGE("Bad boot message\n\"%.20s\"\n", boot.recovery);
        }
    }

    // --- if that doesn't work, try the command file
    if (*argc <= 1) {
        FILE *fp = fopen_root_path(COMMAND_FILE, "r");
        if (fp != NULL) {
            char *argv0 = (*argv)[0];
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = argv0;  // use the same program name

            char buf[MAX_ARG_LENGTH];
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if (!fgets(buf, sizeof(buf), fp)) break;
                (*argv)[*argc] = strdup(strtok(buf, "\r\n"));  // Strip newline.
            }

            check_and_fclose(fp, COMMAND_FILE);
            LOGI("Got arguments from %s\n", COMMAND_FILE);
        }
    }

    // --> write the arguments we have back into the bootloader control block
    // always boot into recovery after this (until finish_recovery() is called)
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    int i;
    for (i = 1; i < *argc; ++i) {
        strlcat(boot.recovery, (*argv)[i], sizeof(boot.recovery));
        strlcat(boot.recovery, "\n", sizeof(boot.recovery));
    }
    set_bootloader_message(&boot);
}


// clear the recovery command and prepare to boot a (hopefully working) system,
// copy our log file to cache as well (for the system to read), and
// record any intent we were asked to communicate back to the system.
// this function is idempotent: call it as many times as you like.
static void
finish_recovery(const char *send_intent)
{
    // By this point, we're ready to return to the main system...
    if (send_intent != NULL) {
        FILE *fp = fopen_root_path(INTENT_FILE, "w");
        if (fp == NULL) {
            LOGE("Can't open %s\n", INTENT_FILE);
        } else {
            fputs(send_intent, fp);
            check_and_fclose(fp, INTENT_FILE);
        }
    }

    // Copy logs to cache so the system can find out what happened.
    FILE *log = fopen_root_path(LOG_FILE, "a");
    if (log == NULL) {
        LOGE("Can't open %s\n", LOG_FILE);
    } else {
        FILE *tmplog = fopen(TEMPORARY_LOG_FILE, "r");
        if (tmplog == NULL) {
            LOGE("Can't open %s\n", TEMPORARY_LOG_FILE);
        } else {
            static long tmplog_offset = 0;
            fseek(tmplog, tmplog_offset, SEEK_SET);  // Since last write
            char buf[4096];
            while (fgets(buf, sizeof(buf), tmplog)) fputs(buf, log);
            tmplog_offset = ftell(tmplog);
            check_and_fclose(tmplog, TEMPORARY_LOG_FILE);
        }
        check_and_fclose(log, LOG_FILE);
    }

    // Reset the bootloader message to revert to a normal main system boot.
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    set_bootloader_message(&boot);

    // Remove the command file, so recovery won't repeat indefinitely.
    char path[PATH_MAX] = "";
    if (ensure_root_path_mounted(COMMAND_FILE) != 0 ||
        translate_root_path(COMMAND_FILE, path, sizeof(path)) == NULL ||
        (unlink(path) && errno != ENOENT)) {
        LOGW("Can't unlink %s\n", COMMAND_FILE);
    }

    sync();  // For good measure.
}

#define TEST_AMEND 0
#if TEST_AMEND
static void
test_amend()
{
    extern int test_symtab(void);
    extern int test_cmd_fn(void);
    int ret;
    LOGD("Testing symtab...\n");
    ret = test_symtab();
    LOGD("  returned %d\n", ret);
    LOGD("Testing cmd_fn...\n");
    ret = test_cmd_fn();
    LOGD("  returned %d\n", ret);
}
#endif  // TEST_AMEND

static int
erase_root(const char *root)
{
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    ui_print("Formatting %s..", root);
    return format_root_device(root);
}

static void
backup_partition(char partition[])
{
	ui_print("\n- This will BACKUP your %s!", partition);
	ui_print("\n- Press HOME to confirm, or");
	ui_print("\n- any other key to abort..");
	int confirm_item = ui_wait_key();
	if (confirm_item == KEY_DREAM_HOME) {
		char path[strlen(partition)];
		if (strcmp(partition, "data") == 0) {
			strcpy(path, "DATA:");
		} else {
			strcpy(path, "SYSTEM:");
		}
	
		if (ensure_root_path_mounted(path) != 0) {
			ui_print("Can't mount %s\n", partition);
		} else if (ensure_root_path_mounted("SDCARD:") != 0) {
			ui_print("Can't mount sdcard\n");
		} else {
		
			ui_print("\nPerforming backup");
		
			//create filename
			time_t rawtime;
			struct tm * timeinfo;
			char formattime[25];
			time ( &rawtime );
			timeinfo = localtime ( &rawtime );
			strftime (formattime,25,"_backup_%y%m%d%H%M%S.tar",timeinfo);
			char filename[strlen(partition)+37];
			strcpy(filename, "/sdcard/");
			strcat(filename, partition);
			strcat(filename, formattime);
		
			//create exclude command
			char exclude[strlen(partition)+26];
			strcpy(exclude, "--exclude='");
			strcat(exclude, partition);
			strcat(exclude, "/$RFS_LOG.LO$'");
	
			int error=0;
			pid_t pid = fork();
			if (pid == 0) {
				error=execl("/sbin/busybox", "busybox", "tar", "-c", exclude, "-f", filename, partition, NULL);
				_exit(-1);
			}
		
			int status;
		
			while (waitpid(pid, &status, WNOHANG) == 0) {
				ui_print(".");
				sleep(1);
			}
			ui_print("\n");
		
			if (error != 0) {
				ui_print("Error creating backup. Backup not performed.\n\n");
			} else {
				ui_print("Backup %s complete!\n", partition);
			}
		}
	} else {
		ui_print("\nBackup %s aborted.\n", partition);
	}
}

static void
restore_partition(char partition[])
{
    static char* headers[] = {  "",
    							"",
    							"",
    							"Choose backup file to restore",
    							"",
    							"Use Up/Down and OK to select",
    							"Back returns to data options",
    							"",
    							NULL };

    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    char **files;
    int total = 0;
    int i;

    if (ensure_root_path_mounted(SDCARD_PATH) != 0) {
        LOGE("Can't mount %s\n", SDCARD_PATH);
        return;
    }

    if (translate_root_path(SDCARD_PATH, path, sizeof(path)) == NULL) {
        LOGE("Bad path %s", path);
        return;
    }

    dir = opendir(path);
    if (dir == NULL) {
        LOGE("Couldn't open directory %s", path);
        return;
    }
    
    char prefix[strlen(partition)+9];
    strcpy(prefix, partition);
    strcat(prefix, "_backup_");

    /* count how many files we're looking at */
    while ((de = readdir(dir)) != NULL) {        
        if (strncmp(de->d_name, prefix, strlen(prefix)) == 0){
        	total++;
        }
    }

    /* allocate the array for the file list menu */
    files = (char **) malloc((total + 1) * sizeof(*files));
    files[total] = NULL;

    /* set it up for the second pass */
    rewinddir(dir);

    /* put the names in the array for the menu */
    i = 0;
    while ((de = readdir(dir)) != NULL) {        
        if (strncmp(de->d_name, prefix, strlen(prefix)) == 0){
        	files[i] = (char *) malloc(strlen(de->d_name) + 1);
            strcpy(files[i], de->d_name);
            i++;
        }
    }

    /* close directory handle */
    if (closedir(dir) < 0) {
        LOGE("Failure closing directory %s", path);
        goto out;
    }

    ui_start_menu(headers, files);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        if (key == KEY_DREAM_BACK) {
            break;
        } else if ((key == KEY_DOWN || key == KEY_DREAM_VOLUMEDOWN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == KEY_UP || key == KEY_DREAM_VOLUMEUP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if (key == KEY_I5700_CENTER && visible) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            ui_print("\n- This will RESTORE your %s!",partition);
                    ui_print("\n- Press HOME to confirm, or");
                    ui_print("\n- any other key to abort..");
                    int confirm_item = ui_wait_key();
                    if (confirm_item == KEY_DREAM_HOME) {
                    	ui_print("\n");
                    	if (strcmp(partition, "data") == 0) {
                    		strcpy(prefix, "DATA:");
                    	} else {
                    		strcpy(prefix, "SYSTEM:");
                    	}
                    	erase_root(prefix);
                        ui_print("Performing restore");
                        if (ensure_root_path_mounted(prefix) != 0) {
            				ui_print("Can't mount %s\n", partition);
            			} else {
                        
                        char filename[strlen(files[chosen_item]) + 9];
                        strcpy(filename, "/sdcard/");
                		strcat(filename, files[chosen_item]);
                        
                        int error=0;
                        pid_t pid = fork();
                        if (pid == 0) {
                            error=execl("/sbin/busybox", "busybox", "tar", "-x", "-f", filename, NULL);
                            _exit(-1);                           
                        }

                        int status;

                        while (waitpid(pid, &status, WNOHANG) == 0) {
                            ui_print(".");
                            sleep(1);
                        }
                        ui_print("\n");

                        //if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
                        if (error != 0){
                             ui_print("Error restoring %s. Restore not performed.\n\n", partition);
                        } else {
                             ui_print("Restore %s complete!\n",partition);
                        }
                        }
                   
                    } else {
                        ui_print("\nRestore %s aborted.\n", partition);
                    }
            
            if (!ui_text_visible()) break;
            break;
        }
    }

out:

    for (i = 0; i < total; i++) {
        free(files[i]);
    }
    free(files);
}

static void
sdcard_options()
{
    static char* headers[] = { 	"",
    							"",
    							"",
    							"         Sdcard Options",
                        		"",
                        		"Use Up/Down and OK to select",
                        		"Back returns to main menu",
                        		"",
                        		NULL };

#define SDCARD_MOUNT	 	0
#define SDCARD_UNMOUNT 		1
#define SDCARD_HOST_MOUNT 	2
#define SDCARD_HOST_UNMOUNT 	3


    static char* items[] = { 	"Mount to /sdcard",
								"Unmount from /sdcard",
								"Mount to USB",
								"Unmount from USB",
			 			     	NULL };


    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();
        
        if (key == KEY_DREAM_BACK) {
            break;
        } else if ((key == KEY_DREAM_VOLUMEDOWN || key == KEY_DOWN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == KEY_DREAM_VOLUMEUP || key == KEY_UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if (key == KEY_I5700_CENTER && visible) {
            chosen_item = selected;
        }
        
        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();
            
           int confirm_item;
            switch (chosen_item) {
                case SDCARD_MOUNT:
			if (ensure_root_path_mounted("SDCARD:") != 0) {
            			ui_print("\nCan't mount sdcard\n");
            		}
            		else {
            			ui_print("\nSdcard mounted to /sdcard\n");
            		}
		break;
				
		case SDCARD_UNMOUNT:
			if (ensure_root_path_unmounted("SDCARD:") != 0) {
            			ui_print("\nCan't unmount sdcard\n");
            		}
            		else {
            			ui_print("\nSdcard unmounted from /sdcard\n");
            		}
		break;

		case SDCARD_HOST_MOUNT:
			if (ensure_root_path_mounted("SDCARD:") != 0) {
            				ui_print("\nCan't mount Sdcard on USB\n");
            			} else {
                    		
                    		int error=0;
                    		//error = system("/sbin/busybox rm /data/dalvik-cache/*");
                    		error = system("/sbin/busybox echo /dev/block/vold/179:0 > /sys/devices/platform/s3c6410-usbgadget/gadget/lun0/file");
                        	ui_print("\n");

                        	if (error != 0){
                             	ui_print("\nError mounting sdcard to USB.\n\n");
                        	} else {
                             	ui_print("\nSdcard mounted to USB\n");
                        	}
                        }
					break;

		case SDCARD_HOST_UNMOUNT:
			if (ensure_root_path_mounted("SDCARD:") != 0) {
            				ui_print("\nCan't unmount Sdcard on USB\n");
            			} else {
                    		
                    		int error=0;
                    		//error = system("/sbin/busybox rm /data/dalvik-cache/*");
                    		error = system("/sbin/busybox echo > /sys/devices/platform/s3c6410-usbgadget/gadget/lun0/file");
                        	ui_print("\n");

                        	if (error != 0){
                             	ui_print("\nError unmounting sdcard from USB\n");
                        	} else {
                             	ui_print("\nSdcard unmounted from USB\n");
                        	}
                        }
					break;
			}
			
            // if we didn't return from this function to reboot, show
            // the menu again.
            ui_start_menu(headers, items);
            selected = 0;
            chosen_item = -1;

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
           
		}
		
	}
}

static void
system_options()
{
    static char* headers[] = { 	"",
    							"",
    							"",
    							"         System Options",
                        		"",
                        		"Use Up/Down and OK to select",
                        		"Back returns to main menu",
                        		"",
                        		NULL };

#define SYSTEM_BACKUP		0
#define SYSTEM_RESTORE		1
#define SYSTEM_MOUNT	 	2
#define SYSTEM_UNMOUNT 		3

    static char* items[] = { 	"Backup",
			     				"Restore",
			     				"Mount",
								"Unmount",
			 			     	NULL };

    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();
        
        if (key == KEY_DREAM_BACK) {
            break;
        } else if ((key == KEY_DREAM_VOLUMEDOWN || key == KEY_DOWN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == KEY_DREAM_VOLUMEUP || key == KEY_UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if (key == KEY_I5700_CENTER && visible) {
            chosen_item = selected;
        }
        
        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();
            
            int confirm_item;
            switch (chosen_item) {
                case SYSTEM_BACKUP:
                	backup_partition("system");
                    break;

				case SYSTEM_RESTORE:
					restore_partition("system");
					break;
				
				case SYSTEM_MOUNT:
					if (ensure_root_path_mounted("SYSTEM:") != 0) {
            			ui_print("\nCan't mount system\n");
            		}
            		else {
            			ui_print("\nSystem mounted\n");
            		}
					break;
				
				case SYSTEM_UNMOUNT:
					if (ensure_root_path_unmounted("SYSTEM:") != 0) {
            			ui_print("\nCan't unmount system\n");
            		}
            		else {
            			ui_print("\nSystem unmounted\n");
            		}
					break;
			}
			
            // if we didn't return from this function to reboot, show
            // the menu again.
            ui_start_menu(headers, items);
            selected = 0;
            chosen_item = -1;

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
           
		}
		
	}
}

static void
data_options()
{
    static char* headers[] = { 	"",
    							"",
    							"",
    							"          Data Options",
                        		"",
                        		"Use Up/Down and OK to select",
                        		"Back returns to main menu",
                        		"",
                        		NULL };

#define DATA_BACKUP			0
#define DATA_RESTORE		1
#define DATA_CLEAR_DALVIK	2
#define DATA_WIPE	 		3
#define DATA_MOUNT	 		4
#define DATA_UNMOUNT 		5

    static char* items[] = { 	"Backup",
			     				"Restore",
			     				"Clear dalvik cache",
								"Wipe/factory reset",
								"Mount",
								"Unmount",
			 			     	NULL };


    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();
        
        if (key == KEY_DREAM_BACK) {
            break;
        } else if ((key == KEY_DREAM_VOLUMEDOWN || key == KEY_DOWN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == KEY_DREAM_VOLUMEUP || key == KEY_UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if (key == KEY_I5700_CENTER && visible) {
            chosen_item = selected;
        }
        
        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();
            
            int confirm_item;
            switch (chosen_item) {
                case DATA_BACKUP:
                	backup_partition("data");
                    break;

				case DATA_RESTORE:
					restore_partition("data");
					break;
				
				case DATA_CLEAR_DALVIK:
					ui_print("\n- This will ERASE dalvik cache!");
                    ui_print("\n- Press HOME to confirm, or");
                    ui_print("\n- any other key to abort..");
                    confirm_item = ui_wait_key();
                    if (confirm_item == KEY_DREAM_HOME) {
                    	if (ensure_root_path_mounted("DATA:") != 0) {
            				ui_print("\nCan't mount data\n");
            			} else {
                    		ui_print("\nClearing dalvik cache");
                    		
                    		int error=0;
                    		error = system("/sbin/busybox rm /data/dalvik-cache/*");
                    		
                        	//pid_t pid = fork();
                        	//if (pid == 0) {
                            	//error=execl("/bin/sh", "sh", "-c", "busybox", "rm", "/data/dalvik-cache/*", NULL);
                            	//_exit(-1);
                        	//}

                        	//int status;

                        	//while (waitpid(pid, &status, WNOHANG) == 0) {
                            	//ui_print(".");
                            	//sleep(1);
                        	//}
                        	ui_print("\n");

                        	if (error != 0){
                             	ui_print("Error clearing dalvik cache. Cache not cleared.\n\n");
                        	} else {
                             	ui_print("Dalvik cache cleared!\n");
                        	}
                        }
                   
                    } else {
                        ui_print("\nClear dalvik cache aborted.\n");
                    }
					break;
				
				case DATA_WIPE:
                    ui_print("\n- This will ERASE your data!");
                    ui_print("\n- Press HOME to confirm, or");
                    ui_print("\n- any other key to abort..");
                    confirm_item = ui_wait_key();
                    if (confirm_item == KEY_DREAM_HOME) {
                        ui_print("\nWiping data...\n");
                        erase_root("DATA:");
                        erase_root("CACHE:");
                        ui_print("Data wipe complete.\n");
                    } else {
                        ui_print("\nData wipe aborted.\n");
                    }
                    break;
				
				case DATA_MOUNT:
					if (ensure_root_path_mounted("DATA:") != 0) {
            			ui_print("\nCan't mount data\n");
            		}
            		else {
            			ui_print("\nData mounted\n");
            		}
					break;
				
				case DATA_UNMOUNT:
					if (ensure_root_path_unmounted("DATA:") != 0) {
            			ui_print("\nCan't unmount data\n");
            		}
            		else {
            			ui_print("\nData unmounted\n");
            		}
					break;
			}
			
            // if we didn't return from this function to reboot, show
            // the menu again.
            ui_start_menu(headers, items);
            selected = 0;
            chosen_item = -1;

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
		}
		
	}
}

static void
flash_options()
{
    static char* headers[] = { 	"",
    							"",
    							"",
    							"          Flash Options",
                        		"",
                        		"Use Up/Down and OK to select",
                        		"Back returns to main menu",
                        		"",
                        		NULL };

#define FLASH_KERNEL		0
#define FLASH_LOGO		1
#define FLASH_RECOVERY		2

    static char* items[] = { 	"Kernel (zImage)",
			     				"Boot Screen (logo.png)",
			     				"Recovery (recovery.rfs)",
			 			     	NULL };


    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();
        
        if (key == KEY_DREAM_BACK) {
            break;
        } else if ((key == KEY_DREAM_VOLUMEDOWN || key == KEY_DOWN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == KEY_DREAM_VOLUMEUP || key == KEY_UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if (key == KEY_I5700_CENTER && visible) {
            chosen_item = selected;
        }
        
        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();
            
            int confirm_item;
            switch (chosen_item) {
                
                  // old kernel flash command
                  //case FLASH_KERNEL:
                	//backup_partition("data");
                  //break;
                  

		case FLASH_KERNEL:
			ui_print("\n- This will FLASH a new Kernel!");
			ui_print("\n- Kernel must be named zImage");
			ui_print("\n- zImage must be in /sdcard/updates/");
                        ui_print("\n- Press HOME to confirm, or");
         		ui_print("\n- any other key to abort..");
                    confirm_item = ui_wait_key();
                    if (confirm_item == KEY_DREAM_HOME) {
                       // make sure zImage exists in /sdcard/zImage
                       // if (ensure_root_path_mounted("DATA:") != 0) {
            	     //		ui_print("\nCan't mount data\n");
            	     //	} else {
                       //	ui_print("\nClearing dalvik cache");
                       // }
                    		int error=0;
                        	pid_t pid = fork();
                        	if (pid == 0) {
                            	error=execl("/sbin/flash_image", "flash_image", "boot", "/sdcard/updates/zImage", NULL);
                            	_exit(-1);
                        	}

                        	int status;

                        	while (waitpid(pid, &status, WNOHANG) == 0) {
                            	ui_print(".");
                            	sleep(1);
                        	}
                        	ui_print("\n");

                        	if (error != 0){
                             	ui_print("Error flashing Kernel - zImage.\n\n");
                        	} else {
                             	ui_print("\nKernel - zImage flashed successfully!\n"); 
				ui_print("Reboot for changes to take effect!\n");
                        	}
                 
                    // for confirm item button pressed anything other than Home
		    } else {
                        ui_print("\nFlashing of Kernel aborted.\n");
                    }
		break;
 
		case FLASH_LOGO:
			ui_print("\n- This will FLASH a new Boot Screen!");
			ui_print("\n- Improper use has permanently BRICKED phones!");
			ui_print("\n- Boot screen must be named logo.png"); 				ui_print("\n- logo.png must be in /sdcard/updates/");
                        ui_print("\n- Press HOME to confirm, or");
         		ui_print("\n- any other key to abort..");
                    confirm_item = ui_wait_key();
                    if (confirm_item == KEY_DREAM_HOME) {
                       // make sure zImage exists in /sdcard/zImage
                       // if (ensure_root_path_mounted("DATA:") != 0) {
            	     //		ui_print("\nCan't mount data\n");
            	     //	} else {
                       //	ui_print("\nClearing dalvik cache");
                       // }
                    		int error=0;
                        	pid_t pid = fork();
                        	if (pid == 0) {
                            	error=execl("/sbin/flash_image", "flash_image", "boot3", "/sdcard/updates/logo.png", NULL);
                            	_exit(-1);
                        	}

                        	int status;

                        	while (waitpid(pid, &status, WNOHANG) == 0) {
                            	ui_print(".");
                            	sleep(1);
                        	}
                        	ui_print("\n");

                        	if (error != 0){
                             	ui_print("Error flashing Boot Screen - logo.png.\n\n");
                        	} else {
                             	ui_print("\nBoot Screen - logo.png flashed successfully!\n"); 
				ui_print("Reboot for changes to take effect!\n");
                        	}
                 
                    // for confirm item button pressed anything other than Home
		    } else {
                        ui_print("\nFlashing of Boot Screen aborted.\n");
                    }
		break;
				
		case FLASH_RECOVERY:
			ui_print("\n- This will ERASE the current Recovery!");
			ui_print("\n- Recovery image must be named recovery.rfs");
	 		ui_print("\n- recovery.rfs must be in /sdcard/updates/");
		        ui_print("\n- Press HOME to confirm, or");
                        ui_print("\n- any other key to abort..");

                    confirm_item = ui_wait_key();
                    if (confirm_item == KEY_DREAM_HOME) {
                       // make sure recovery.rfs exists in /sdcard/recovery.rfs
                    	// if (ensure_root_path_mounted("DATA:") != 0) {
            		//		ui_print("\nCan't mount data\n");
            		//	} else {
                    	//	ui_print("\nClearing dalvik cache");
                        //}
                    		
                    		int error=0;
                        	pid_t pid = fork();
                        	if (pid == 0) {
                            	error=execl("/sbin/flash_image", "flash_image", "recovery", "/sdcard/updates/recovery.rfs", NULL);
                            	_exit(-1);
                        	}

                        	int status;

                        	while (waitpid(pid, &status, WNOHANG) == 0) {
                            	ui_print(".");
                            	sleep(1);
                        	}
                        	ui_print("\n");

                        	if (error != 0){
                             	ui_print("Error flashing Recovery - recovery.rfs.\n\n");
                        	} else {
                             	ui_print("\nRecovery - recovery.rfs flashed successfully!\n"); 
				ui_print("Reboot for changes to take effect!\n");
                        	}

                    // for mount on confirm item button pressed anything other than Home
		    } else {
                        ui_print("\nFlashing of Recovery aborted.\n");
                    }

					break;
		}
            // if we didn't return from this function to reboot, show
            // the menu again.
            ui_start_menu(headers, items);
            selected = 0;
            chosen_item = -1;

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
		}
		
	}
}


static void
choose_update_file()
{
    static char* headers[] = {  "",
								"",
    							"",
    							"Choose update ZIP file",
    							"",
    							"Use Up/Down and OK to select",
                        		"Back returns to main menu",
    							"",
                        		NULL };

    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    char **files;
    int total = 0;
    int i;

    if (ensure_root_path_mounted(SDCARD_PATH) != 0) {
        LOGE("Can't mount %s\n", SDCARD_PATH);
        return;
    }

    if (translate_root_path(SDCARD_PATH, path, sizeof(path)) == NULL) {
        LOGE("Bad path %s", path);
        return;
    }

    dir = opendir(path);
    if (dir == NULL) {
        LOGE("Couldn't open directory %s", path);
        return;
    }

    /* count how many files we're looking at */
    while ((de = readdir(dir)) != NULL) {
        char *extension = strrchr(de->d_name, '.');
        if (extension == NULL || de->d_name[0] == '.') {
            continue;
        } else if (!strcasecmp(extension, ".zip")) {
            total++;
        }
    }

    /* allocate the array for the file list menu */
    files = (char **) malloc((total + 1) * sizeof(*files));
    files[total] = NULL;

    /* set it up for the second pass */
    rewinddir(dir);

    /* put the names in the array for the menu */
    i = 0;
    while ((de = readdir(dir)) != NULL) {
        char *extension = strrchr(de->d_name, '.');
        if (extension == NULL || de->d_name[0] == '.') {
            continue;
        } else if (!strcasecmp(extension, ".zip")) {
            files[i] = (char *) malloc(SDCARD_PATH_LENGTH + strlen(de->d_name) + 1);
            strcpy(files[i], de->d_name);
            i++;
        }
    }

    /* close directory handle */
    if (closedir(dir) < 0) {
        LOGE("Failure closing directory %s", path);
        goto out;
    }

    ui_start_menu(headers, files);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        if (key == KEY_DREAM_BACK) {
            break;
        } else if ((key == KEY_DOWN || key == KEY_DREAM_VOLUMEDOWN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == KEY_UP || key == KEY_DREAM_VOLUMEUP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if (key == KEY_I5700_CENTER && visible) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            ui_print("\n- Installing new image!");
            ui_print("\n- Press HOME to confirm, or");
            ui_print("\n- any other key to abort..");
            int confirm_apply = ui_wait_key();
            if (confirm_apply == KEY_DREAM_HOME) {
                ui_print("\nInstall from sdcard...\n");
                char package_name[SDCARD_PATH_LENGTH + strlen(files[chosen_item]) + 1];
                strcpy(package_name, SDCARD_PATH);
                strcat(package_name, files[chosen_item]);
                int status = install_package(package_name);
                if (status != INSTALL_SUCCESS) {
                    ui_set_background(BACKGROUND_ICON_ERROR);
                    ui_print("Installation aborted.\n");
                } else if (!ui_text_visible()) {
                    break;  // reboot if logs aren't visible
                } else {
                    if (firmware_update_pending()) {
                        ui_print("\nReboot via home+back or menu\n"
                                 "to complete installation.\n");
                    } else {
                        ui_print("\nInstall from sdcard complete.\n");
                    }
                }
            } else {
                ui_print("\nInstallation aborted.\n");
            }
            if (!ui_text_visible()) break;
            break;
        }
    }

out:

    for (i = 0; i < total; i++) {
        free(files[i]);
    }
    free(files);
}

//32 character length
static void
prompt_and_wait()
{
    static char* headers[] = { 	"",
    							"",
    							"",
    							" Android System Recovery "
            	              	EXPAND(RECOVERY_API_VERSION) "",
                		        "  SDX Samsung Moment SPH-M900",
								"",
                        		"Use Up/Down and OK to select",
                        		"",
                        		NULL };

// these constants correspond to elements of the items[] list.
#define ITEM_REBOOT			0
#define ITEM_APPLY_ZIP		1
#define ITEM_DATA_OPTIONS	2
#define ITEM_SYSTEM_OPTIONS	3
#define ITEM_SDCARD_OPTIONS	4
#define ITEM_FLASH_OPTIONS	5
#define ITEM_CONSOLE       	6

	static char* items[] = {"Reboot system now",
							"Apply zip from Sdcard",
							"Data options",
							"System options",
							"Sdcard options",
							"Flash options",
							"Go to Console",
							NULL };

    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        if ((key == KEY_DREAM_VOLUMEDOWN || key == KEY_DOWN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == KEY_DREAM_VOLUMEUP || key == KEY_UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if (key == KEY_I5700_CENTER && visible) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            switch (chosen_item) {
                case ITEM_REBOOT:
                    return;

                case ITEM_APPLY_ZIP:
                    choose_update_file();
                    break;
                    
                case ITEM_DATA_OPTIONS:
                	data_options();
                	break;
                	
                case ITEM_SYSTEM_OPTIONS:
                	system_options();
                	break;
                	
                case ITEM_SDCARD_OPTIONS:
                	sdcard_options();
                	break;

                case ITEM_FLASH_OPTIONS:
                	flash_options();
                	break;

                case ITEM_CONSOLE:
                    ui_print("\nGoing to the Console!\n");
		    do_reboot = 0;
                    gr_exit();
                    break;
            }

            // if we didn't return from this function to reboot, show
            // the menu again.
            ui_start_menu(headers, items);
            selected = 0;
            chosen_item = -1;

            finish_recovery(NULL);
            ui_reset_progress();

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
        }
    }
}

static void
print_property(const char *key, const char *name, void *cookie)
{
    fprintf(stderr, "%s=%s\n", key, name);
}

int
main(int argc, char **argv)
{
    time_t start = time(NULL);

    // If these fail, there's not really anywhere to complain...
    freopen(TEMPORARY_LOG_FILE, "a", stdout); setbuf(stdout, NULL);
    freopen(TEMPORARY_LOG_FILE, "a", stderr); setbuf(stderr, NULL);
    fprintf(stderr, "Starting recovery on %s", ctime(&start));

    tcflow(STDIN_FILENO, TCOOFF);
    
//    char prop_value[PROPERTY_VALUE_MAX];
//    property_get("ro.build.display.id", &prop_value, "not set");

    ui_init();
//    ui_print("Build: ");
//    ui_print(prop_value);
//    ui_print("\n    by LeshaK (forum.leshak.com)\n\n");
    get_args(&argc, &argv);

    int previous_runs = 0;
    const char *send_intent = NULL;
    const char *update_package = NULL;
    int wipe_data = 0, wipe_cache = 0;

    int arg;
    while ((arg = getopt_long(argc, argv, "", OPTIONS, NULL)) != -1) {
        switch (arg) {
        case 'p': previous_runs = atoi(optarg); break;
        case 's': send_intent = optarg; break;
        case 'u': update_package = optarg; break;
        case 'w': wipe_data = wipe_cache = 1; break;
        case 'c': wipe_cache = 1; break;
        case '?':
            LOGE("Invalid command argument\n");
            continue;
        }
    }

    fprintf(stderr, "Command:");
    for (arg = 0; arg < argc; arg++) {
        fprintf(stderr, " \"%s\"", argv[arg]);
    }
    fprintf(stderr, "\n\n");

    property_list(print_property, NULL);
    fprintf(stderr, "\n");

#if TEST_AMEND
    test_amend();
#endif

    RecoveryCommandContext ctx = { NULL };
    if (register_update_commands(&ctx)) {
        LOGE("Can't install update commands\n");
    }

    int status = INSTALL_SUCCESS;

    if (update_package != NULL) {
        status = install_package(update_package);
        if (status != INSTALL_SUCCESS) ui_print("Installation aborted.\n");
    } else if (wipe_data || wipe_cache) {
        if (wipe_data && erase_root("DATA:")) status = INSTALL_ERROR;
        if (wipe_cache && erase_root("CACHE:")) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS) ui_print("Data wipe failed.\n");
    } else {
        status = INSTALL_ERROR;  // No command specified
    }

    if (status != INSTALL_SUCCESS) ui_set_background(BACKGROUND_ICON_ERROR);
    if (status != INSTALL_SUCCESS) prompt_and_wait();

    // If there is a radio image pending, reboot now to install it.
    // maybe_install_firmware_update(send_intent);

    // Otherwise, get ready to boot the main system...
    finish_recovery(send_intent);
    sync();
    if (do_reboot)
    {
    	ui_print("Rebooting...\n");
	sync();
    	reboot(RB_AUTOBOOT);
	}
	
	tcflush(STDIN_FILENO, TCIOFLUSH);	
	tcflow(STDIN_FILENO, TCOON);
	
    return EXIT_SUCCESS;
}

