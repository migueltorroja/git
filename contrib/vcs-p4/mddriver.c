/* MDDRIVER.C - test driver for MD2, MD4 and MD5
*/

/* Copyright (C) 1990-2, RSA Data Security, Inc. Created 1990. All
   rights reserved.

   RSA Data Security, Inc. makes no representations concerning either
   the merchantability of this software or the suitability of this
   software for any particular purpose. It is provided "as is"
   without express or implied warranty of any kind.

   These notices must be retained in any copies of any part of this
   documentation and/or software.
   */

/* The following makes MD default to MD5 if it has not already been
   defined with C compiler flags.
   */

#include "git-compat-util.h"
#include "md5.h"

/* Length of test block, number of test blocks.
*/
#define TEST_BLOCK_LEN 1000
#define TEST_BLOCK_COUNT 1000

static void MDString (char *);
static void MDTimeTrial (void);
static void MDTestSuite (void);
static void MDFile (char *);
static void MDFilter (void);
static void MDPrint (uint8_t [16]);

#define MD_CTX MD5_CTX
#define MDInit MD5Init
#define MDUpdate MD5Update
#define MDFinal MD5Final

/* Main driver.

   Arguments (may be any combination):
   -sstring - digests string
   -t       - runs time trial
   -x       - runs test script
   filename - digests file
   (none)   - digests standard input
   */
int main (argc, argv)
	int argc;
	char *argv[];
{
	int i;

	if (argc > 1)
		for (i = 1; i < argc; i++)
			if (argv[i][0] == '-' && argv[i][1] == 's')
				MDString (argv[i] + 2);
			else if (strcmp (argv[i], "-t") == 0)
				MDTimeTrial ();
			else if (strcmp (argv[i], "-x") == 0)
				MDTestSuite ();
			else
				MDFile (argv[i]);
	else
		MDFilter ();

	return (0);
}

/* Digests a string and prints the result.
*/
static void MDString (string)
	char *string;
{
	MD_CTX context;
	uint8_t digest[16];
	size_t len = strlen (string);

	MDInit (&context);
	MDUpdate (&context, (uint8_t *)string, len);
	MDFinal (digest, &context);

	printf ("MD5 (\"%s\") = ", string);
	MDPrint (digest);
	printf ("\n");
}

/* Measures the time to digest TEST_BLOCK_COUNT TEST_BLOCK_LEN-byte
   blocks.
   */
static void MDTimeTrial ()
{
	MD_CTX context;
	time_t endTime, startTime;
	uint8_t block[TEST_BLOCK_LEN], digest[16];
	size_t i;

	printf
		("MD5 time trial. Digesting %d %d-byte blocks ...",
		 TEST_BLOCK_LEN, TEST_BLOCK_COUNT);

	/* Initialize block */
	for (i = 0; i < TEST_BLOCK_LEN; i++)
		block[i] = (unsigned char)(i & 0xff);

	/* Start timer */
	time (&startTime);

	/* Digest blocks */
	MDInit (&context);
	for (i = 0; i < TEST_BLOCK_COUNT; i++)
		MDUpdate (&context, block, TEST_BLOCK_LEN);
	MDFinal (digest, &context);

	/* Stop timer */
	time (&endTime);

	printf (" done\n");
	printf ("Digest = ");
	MDPrint (digest);
	printf ("\nTime = %ld seconds\n", (long)(endTime-startTime));
	printf
		("Speed = %ld bytes/second\n",
		 (long)TEST_BLOCK_LEN * (long)TEST_BLOCK_COUNT/(endTime-startTime));
}

/* Digests a reference suite of strings and prints the results.
*/
static void MDTestSuite ()
{
	printf ("MD5 test suite:\n");

	MDString("");
	MDString("a");
	MDString("abc");
	MDString("message digest");
	MDString("abcdefghijklmnopqrstuvwxyz");
	MDString("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
	MDString("1234567890123456789012345678901234567890"
		"1234567890123456789012345678901234567890");
}

/* Digests a file and prints the result.
*/
static void MDFile (filename)
	char *filename;
{
	FILE *file;
	MD_CTX context;
	size_t len;
	uint8_t buffer[1024], digest[16];

	if ((file = fopen (filename, "rb")) == NULL)
		printf ("%s can't be opened\n", filename);

	else {
		MDInit (&context);
		while ((len = fread (buffer, 1, 1024, file)))
			MDUpdate (&context, buffer, len);
		MDFinal (digest, &context);

		fclose (file);

		printf ("MD5 (%s) = ", filename);
		MDPrint (digest);
		printf ("\n");
	}
}

/* Digests the standard input and prints the result.
*/
static void MDFilter ()
{
	MD_CTX context;
	size_t len;
	uint8_t buffer[16], digest[16];

	MDInit (&context);
	while ((len = fread (buffer, 1, 16, stdin)))
		MDUpdate (&context, buffer, len);
	MDFinal (digest, &context);

	MDPrint (digest);
	printf ("\n");
}

/* Prints a message digest in hexadecimal.
*/
static void MDPrint (digest)
	uint8_t digest[16];
{
	unsigned int i;

	for (i = 0; i < 16; i++)
		printf ("%02x", digest[i]);
}

