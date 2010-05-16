#include <stdio.h>
#include <stdlib.h>

#include "bootloader.h"
#include "commands.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"

int
main(int argc, char **argv)
{
	if (argc != 2) {
		printf("Usage: %s zip_file\n", argv[0]);
		return -1;
	}

	FILE *fp = fopen(argv[1], "r");
	if (!fp) {
		printf("Unable to open %s\n", argv[1]);
		return -1;
	}
	fclose(fp);

	RecoveryCommandContext ctx = { NULL };
    	if (register_update_commands(&ctx)) {
        	printf("Can't install update commands\n");
		return -1;
    	}

	if (install_package(argv[1]) {
		printf("Unable to install the package!\n");
		return -1;
	}

	printf("Installed package successfully!\n");
	return 0;
}

