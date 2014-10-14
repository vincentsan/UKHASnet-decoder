#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>
#include <unistd.h> // getopt

// Ukhas protocol layer 1: https://www.ukhas.net/wiki/protocol_details
#define BIT_RATE 2000

// CRC computation start value
// Warning: there are many falvours of CRC-CCITT: XModem, 0xFFFF, 0x1D0F, Kermit
// 0x1D0F seems to be the one uses by the RFM69 radios
#define CRC_START 0x1D0F

// maximum packet size, not including length byte nor CRC checksum
#define MAX_PACKET_SIZE 255

// preamble is not used now (only for debugging)
#define PREAMBLE 0xAAAA

// 2 bytes sync words according to https://github.com/UKHASnet/UKHASnet_Firmware/blob/master/arduino_sensor/RFM69Config.h
// But 5 bytes according to Layer 2 protocl (https://www.ukhas.net/wiki/protocol_details) (if so, simply use a uint64_t sync buffer)
// TODO: add sync tolerance (maybe with preamble to be more robust)
#define SYNC_WORD 0x2DAA
uint16_t syncBuffer;

// Output more details (for debug)
bool verbose = false;

// Default value assumes a 64kHz sampling rate
uint16_t sampleRate = 64000;

// byte buffers used when sync word has been recognized
uint8_t byte;
uint8_t buffer[MAX_PACKET_SIZE];

// frequency shift threshold automatically computed with a sample moving averge over a 8 bit window
int16_t threshold = 0;

// indicates that we are currently synchronized with bit stream for packet decoding
bool packetSync = false;

/////////////// PACKET parsing //////////////
// TODO refactor in another class

// packet length (without length byte)
int len = -1;

// packet offset
int offset = 0;

// packet crc16 computed on the fly
uint16_t computedCrc = 0x1D0F;

// packet crc read
uint16_t readCrc = 0;

// see http://www.atmel.com/webdoc/AVRLibcReferenceManual/group__util__crc_1gaca726c22a1900f9bad52594c8846115f.html
uint16_t crc_xmodem_update (uint16_t crc, uint8_t data) {
  crc = crc ^ ((uint16_t) data << 8);
  for (int i=0; i<8; i++) {
    if (crc & 0x8000) {
      crc = (crc << 1) ^ 0x1021;
    } else {
      crc <<= 1;
    }
  }
  return crc;
}

// print to terminal with time tag
void printTime() {
  char buff[100];
  time_t now = time(0);
  strftime(buff, 100, "%Y-%m-%d %H:%M:%S", localtime(&now));
  printf("%s ", buff);
}

bool processByte(uint8_t byte) {
  if (len == -1) {
    // read length (does not account for length byte)
    len = (int) byte;
    computedCrc = crc_xmodem_update(computedCrc, byte);
    if (len <= MAX_PACKET_SIZE - 1) {
      if (verbose) {
        printTime();
        printf("Parsing %d bytes\n", len);
      }
      // continue reading packet
      return true;
    } else {
      if (verbose) {
        printTime();
        printf("Length: %d > %d, skip\n", len, MAX_PACKET_SIZE - 1);
      }
      // stop reading packet
      return false;
    }
  } else if (offset < len) {
    // read data
    buffer[offset++] = byte;
    computedCrc = crc_xmodem_update(computedCrc, byte);
    // continue reading packet
    return true;
  } else if (offset == len) {
    // read crc first byte
    readCrc |= ((uint16_t) byte << 8);
    offset++;
    return true;
  } else if (offset == len + 1) {
    // read crc 2nd byte
    readCrc |= byte;
    // TODO
    // I don't know why I need to invert this CRC to work
    // bits could be all inverted but not just CRC...
    readCrc = 0xffff - readCrc;
    if (computedCrc == readCrc) {
      printTime();
      printf("PACKET ");
      for (int i=0; i<len; ++i) {
        putchar(buffer[i]);
      }
      printf("\n");
    } else if (verbose) {
      printTime();
      printf("CRC mismatch: read(%04X), computed(%04X)\n", readCrc, computedCrc);
    }
  }
  return false;
}

/////////////// END of PACKET parsing //////////////

int skipBit = 8;

void processBit(bool bit) {
  if (packetSync) {
    // process bits by 8 (byte)
    byte = (byte << 1) | bit;
    if (--skipBit < 1) {
      // reset
      skipBit = 8;
      // process new byte
      packetSync = processByte(byte);
    }
  } else {
    syncBuffer = (syncBuffer << 1) | bit;
    packetSync = (syncBuffer == SYNC_WORD || syncBuffer == 0xffff - SYNC_WORD);
    if (packetSync) {
      // TODO
      // if sync with inverted SYNC_WORD, should invert all bits
      if (verbose) {
        printTime();
        printf("Sync: %04X\n", syncBuffer);
      }
      // start reading bits 8 by 8 (as bytes)
      skipBit = 8;
      // initialize packet
      len = -1;
      offset = 0;
      computedCrc = 0x1D0F;
      readCrc = 0;
    }
  }
}

int main (int argc, char**argv){
  printf("UKHAS decoder using rtl_fm\n");
  // parse options
  int opt;
	while ((opt = getopt(argc, argv, "s:h:v")) != -1) {
		switch (opt) {
      case 's':
        sampleRate = atoi(optarg);
        if (sampleRate < 2 * BIT_RATE || sampleRate % BIT_RATE != 0) {
          fprintf(stderr, "Illegal sampling rate - %d.\n", sampleRate);
          fprintf(stderr, "Must be over %d Hz and a multiple of %d Hz.\n", 2*BIT_RATE, BIT_RATE);
          return 1;
        }
        break;
      case 'v':
        verbose = true;
        break;
      case 'h':
      default:
        printf("Usage: UKHASnet-decoder [-s sample_rate][-v][-h] \n"
                "Expects rtl_fm output:\n"
                "\trtl_fm -f 433961890 -s 64k -g 0 -p 162 | ukhasnet-decoder -v -s 64000\n"
                "\trtl_fm -f 433961890 -s 64k -g 0 -p 162 -r 8000 | ukhasnet-decoder -v -s 8000\n"
                "\t[-s sample_rate in Hz. Above 4kHz and a multiple of 2kHz.]\n"
                "\t[-v verbose mode]\n"
                "\t[-h this help. Check the code if more needed !]\n\n");
        return 0;
        break;
		}
	}
  printf("Sample rate: %d Hz\n", sampleRate);
  if (verbose) {
    printf("Verbose mode\n");
  }
  printf("\n");
	// process samples
  int32_t samples = 0;
  time_t start_time = time(NULL);
  // sample rate / BIT_RATE
  int downSamples = sampleRate / BIT_RATE;
  int skipSamples = downSamples;
	while(!feof(stdin) ) {
    // process 1 sample
		int16_t sample = (int16_t) (fgetc(stdin) | fgetc(stdin) << 8);
    // threshold is a moving average over 8 bits
    // this suppose that packet has sufficient bit transitions
    // TODO find better formula to account for not well balanced packets
    // use all samples and not only subsampled bits
    threshold = (sample + (8 * downSamples - 1) * threshold) / (8 * downSamples);
    // only process 1 bit over g_srate
    if (--skipSamples < 1) {
      // reset
      skipSamples = downSamples;
      // process bit
      bool bit = sample > threshold;
      processBit(bit);
      // to debug more deeply uncomment bellow and comment all other outputs,
      // pipe to a file and plot !
      //printf("%d,%d,%d,%d,%d\n", samples++, sample, threshold, bit ? 6000 : -6000, packetSync ? 6000 : 0);
      //fflush(stdout);
    }
	}
	printf("%d samples in %d sec\n", samples, (int) (time(NULL)-start_time));
	return 0;
}