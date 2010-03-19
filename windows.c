#include "pgreplay.h"
#include <windows.h>
#include <stdio.h>

/* gets the last error and prints an error message */

void win_perror(const char *prefix, int is_network_error) {
	DWORD error_nr;
	char *errmsg;
	/* catalog of Windows socket error messages */
	static HMODULE sock_err_mod = NULL;

	/* get the message number */
	if (is_network_error) {
		error_nr = WSAGetLastError();

		if (NULL == sock_err_mod) {
			/* try to load the Windows socket error message catalog */
			sock_err_mod = LoadLibraryEx(
					"netmsg.dll",
					NULL,
					LOAD_LIBRARY_AS_DATAFILE
				);
		}
	} else {
		error_nr = GetLastError();
	}

	/* get the error message text */
	if (FormatMessage(
			FORMAT_MESSAGE_ALLOCATE_BUFFER
				| FORMAT_MESSAGE_IGNORE_INSERTS
				| FORMAT_MESSAGE_FROM_SYSTEM
				| ((is_network_error && sock_err_mod) ? FORMAT_MESSAGE_FROM_HMODULE : 0),
			sock_err_mod,
			error_nr,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPSTR) &errmsg,
			0,
			NULL)) {
		fprintf(stderr, "%s: %s\n", prefix, errmsg);

		/* free the memory for the error message */
		LocalFree(errmsg);
	} else {
		fprintf(stderr, "%s: error number %ld\n", prefix, error_nr);
	}
}
