//-----------------------------------------------------------------------------
// (c) 2009 Henryk Plötz <henryk@ploetzli.ch>
//     2016 Iceman
//     2018 AntiCat (rwd rewritten)
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// LEGIC RF simulation code
//-----------------------------------------------------------------------------
#include "legicrf.h"

#include "ticks.h"              /* timers */
#include "crc.h"                /* legic crc-4 */
#include "legic_prng.h"         /* legic PRNG impl */
#include "legic.h"              /* legic_card_select_t struct */

static uint8_t* legic_mem;      /* card memory, used for read, write and sim */
static legic_card_select_t card;/* metadata of currently selected card */
static crc_t legic_crc;
static int32_t input_threshold; /* values > threshold are 1 else 0 */

// LEGIC RF is using the common timer functions: StartCountUS() and GetCountUS()
#define RWD_TIME_PAUSE       20 /* 20us */
#define RWD_TIME_1          100 /* READER_TIME_PAUSE 20us off + 80us on = 100us */
#define RWD_TIME_0           60 /* READER_TIME_PAUSE 20us off + 40us on = 60us */
#define TAG_FRAME_WAIT      330 /* 330us from READER frame end to TAG frame start */
#define TAG_BIT_PERIOD      100 /* 100us */

#define LEGIC_CARD_MEMSIZE 1024 /* The largest Legic Prime card is 1k */

//-----------------------------------------------------------------------------
// I/O interface abstraction (FPGA -> ARM)
//-----------------------------------------------------------------------------

static inline uint8_t rx_byte_from_fpga() {
  for(;;) {
    WDT_HIT();

    // wait for byte be become available in rx holding register
    if(AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_RXRDY)) {
      return AT91C_BASE_SSC->SSC_RHR;
    }
  }
}

//-----------------------------------------------------------------------------
// Demodulation
//-----------------------------------------------------------------------------

// Returns am aproximated power measurement
//
// The FPGA running on the xcorrelation kernel samples the subcarrier at ~3 MHz.
// The kernel was initialy designed to receive BSPK/2-PSK. Hance, it reports an
// I/Q pair every 18.9us (8 bits i and 8 bits q).
//
// The subcarrier amplitude can be calculated using Pythagoras sqrt(i^2 + q^2).
// To reduce CPU time the amplitude is approximated by using linear functions:
//   am = MAX(ABS(i),ABS(q)) + 1/2*MIN(ABS(i),ABSq))
//
// Note: The SSC receiver is never synchronized the calculation my be performed
// on a i/q pair from two subsequent correlations, but does not matter.
static inline int32_t sample_power() {
  int32_t q = (int8_t)rx_byte_from_fpga(); q = ABS(q);
  int32_t i = (int8_t)rx_byte_from_fpga(); i = ABS(i);

  return MAX(i, q) + (MIN(i, q) >> 1);
}

// Returns a demedulated bit
//
// An aproximated power measurement is available every 18.9us. The bit time
// is 100us. The code samples 5 times and uses samples 3 and 4.
//
// Note: The demodulator is drifting (18.9us * 5 = 94.5us), since the longest
// respons is 12 bits, the demodulator will stay in sync with a margin of
// error of 20us left. Sending the next request will resync the card.
static inline bool rx_bit() {
  static int32_t p[5];
  for(size_t i = 0; i<5; ++i) {
    p[i] = sample_power();
  }

  if((p[2] > input_threshold) && (p[3] > input_threshold)) {
    return true;
  }
  if((p[2] < input_threshold) && (p[3] < input_threshold)) {
    return false;
  }

  Dbprintf("rx_bit failed %i vs %i (threshold %i)", p[2], p[3], input_threshold);
  return false;
}

//-----------------------------------------------------------------------------
// Modulation
//
// I've tried to modulate the Legic specific pause-puls using ssc and the default
// ssc clock of 105.4 kHz (bit periode of 9.4us) - previous commit. However,
// the timing was not precise enough. By increasing the ssc clock this could
// be circumvented, but the adventage over bitbang would be little.
//-----------------------------------------------------------------------------

static inline void tx_bit(bool bit) {
  uint32_t ts = GetCountUS();

  // insert pause
  LOW(GPIO_SSC_DOUT);
  while(GetCountUS() < ts + RWD_TIME_PAUSE) { };
  HIGH(GPIO_SSC_DOUT);

  // return to high, wait for bit periode to end
  if(bit) {
    while(GetCountUS() < ts + RWD_TIME_1) { };
  } else {
    while(GetCountUS() < ts + RWD_TIME_0) { };
  }
}

//-----------------------------------------------------------------------------
// Frame Handling
//
// The LEGIC RF protocol from card to reader does not include explicit frame
// start/stop information or length information. The reader must know beforehand
// how many bits it wants to receive.
// Notably: a card sending a stream of 0-bits is indistinguishable from no card
// present.
//-----------------------------------------------------------------------------

static void tx_frame(uint32_t frame, uint8_t len) {
  FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER_TX);

  // transmit frame, MSB first
  for(uint8_t i = 0; i < len; ++i) {
    bool bit = (frame >> i) & 0x01;
    tx_bit(bit ^ legic_prng_get_bit());
    legic_prng_forward(1);
  };

  // add pause to mark end of the frame
  uint32_t ts = GetCountUS();
  LOW(GPIO_SSC_DOUT);
  while(GetCountUS() < ts + RWD_TIME_PAUSE) { };
  HIGH(GPIO_SSC_DOUT);
}

static uint32_t rx_frame(uint8_t len) {
  FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER_RX_XCORR
                  | FPGA_HF_READER_RX_XCORR_848_KHZ
                  | FPGA_HF_READER_RX_XCORR_QUARTER);

  uint32_t frame = 0;
  for(uint8_t i = 0; i < len; i++) {
    frame |= (rx_bit() ^ legic_prng_get_bit()) << i;
    legic_prng_forward(1);
  }

  return frame;
}

//-----------------------------------------------------------------------------
// Legic Reader
//-----------------------------------------------------------------------------

int init_card(uint8_t cardtype, legic_card_select_t *p_card) {
  p_card->tagtype = cardtype;

  switch(p_card->tagtype) {
    case 0x0d:
      p_card->cmdsize = 6;
      p_card->addrsize = 5;
      p_card->cardsize = 22;
      break;
    case 0x1d:
      p_card->cmdsize = 9;
      p_card->addrsize = 8;
      p_card->cardsize = 256;
      break;
    case 0x3d:
      p_card->cmdsize = 11;
      p_card->addrsize = 10;
      p_card->cardsize = 1024;
      break;
    default:
      p_card->cmdsize = 0;
      p_card->addrsize = 0;
      p_card->cardsize = 0;
      return 2;
  }
  return 0;
}

static void init_reader(bool clear_mem) {
  // configure FPGA
  FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
  FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER_RX_XCORR
                  | FPGA_HF_READER_RX_XCORR_848_KHZ
                  | FPGA_HF_READER_RX_XCORR_QUARTER);
  SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

  // configure SSC with defaults
  FpgaSetupSsc();

  // re-claim GPIO_SSC_DOUT as GPIO and enable output
  AT91C_BASE_PIOA->PIO_OER = GPIO_SSC_DOUT;
  AT91C_BASE_PIOA->PIO_PER = GPIO_SSC_DOUT;
  HIGH(GPIO_SSC_DOUT);

  // reserve a cardmem, meaning we can use the tracelog function in bigbuff easier.
  legic_mem = BigBuf_get_EM_addr();
  if(legic_mem) {
    memset(legic_mem, 0x00, LEGIC_CARD_MEMSIZE);
  }

  // start trace
  clear_trace();
  set_tracing(true);

  // init crc calculator
  crc_init(&legic_crc, 4, 0x19 >> 1, 0x05, 0);

  // start us timer
  StartCountUS();
}

// Setup reader to card connection
//
// The setup consists of a three way handshake:
//  - Transmit initialisation vector 7 bits
//  - Receive card type 6 bits
//  - Acknowledge frame 6 bits
static uint32_t setup_phase_reader(uint8_t iv) {
  uint32_t ts = GetCountUS();

  // Switch on carrier and let the card charge for 5ms.
  // Use the time to calibrate the treshhold.
  input_threshold = 8; // heuristically determined
  do {
    int32_t sample = sample_power();
    if(sample > input_threshold) {
      input_threshold = sample;
    }
  } while(GetCountUS() < ts + 5000);

  // Set threshold to noise floor * 2
  input_threshold <<= 1;

  legic_prng_init(0);
  tx_frame(iv, 7);
  ts = GetCountUS();

  // configure iv
  legic_prng_init(iv);
  legic_prng_forward(2);

  // wait until card is expect to respond
  while(GetCountUS() < ts + TAG_FRAME_WAIT) { };

  // receive card type
  int32_t card_type = rx_frame(6);

  // send obsfuscated acknowledgment frame
  switch (card_type) {
    case 0x0D:
      tx_frame(0x19, 6); // MIM22 | READCMD = 0x18 | 0x01
      break;
    case 0x1D:
    case 0x3D:
      tx_frame(0x39, 6); // MIM256 | READCMD = 0x38 | 0x01
      break;
  }

  return card_type;
}

static uint8_t calc_crc4(uint16_t cmd, uint8_t cmd_sz, uint8_t value) {
  crc_clear(&legic_crc);
  crc_update(&legic_crc, (value << cmd_sz) | cmd, 8 + cmd_sz);
  return crc_finish(&legic_crc);
}

static int16_t read_byte(uint16_t index, uint8_t cmd_sz) {
  uint16_t cmd = (index << 1) | LEGIC_READ;

  // read one byte
  tx_frame(cmd, cmd_sz);
  uint32_t frame = rx_frame(12);

  // split frame into data and crc
  uint8_t byte = BYTEx(frame, 0);
  uint8_t crc = BYTEx(frame, 1);

  // check received against calculated crc
  uint8_t calc_crc = calc_crc4(cmd, cmd_sz, byte);
  if(calc_crc != crc) {
    Dbprintf("!!! crc mismatch: %x != %x !!!",  calc_crc, crc);
    return -1;
  }

  return byte;
}

//-----------------------------------------------------------------------------
// Command Line Interface
//
// Only this functions are public / called from appmain.c
//-----------------------------------------------------------------------------
void LegicRfInfo(void) {
  // configure ARM and FPGA
  init_reader(false);

  // establish shared secret and detect card type
  uint8_t card_type = setup_phase_reader(0x01);
  if(init_card(card_type, &card) != 0) {
    cmd_send(CMD_ACK, 0, 0, 0, 0, 0);
    goto OUT;
  }

  // read UID
  for(uint8_t i = 0; i < sizeof(card.uid); ++i) {
    int16_t byte = read_byte(i, card.cmdsize);
    if(byte == -1) {
      cmd_send(CMD_ACK, 0, 0, 0, 0, 0);
      goto OUT;
    }
    card.uid[i] = byte & 0xFF;
  }

  // read MCC and check against UID
  int16_t mcc = read_byte(4, card.cmdsize);
  int16_t calc_mcc = CRC8Legic(card.uid, 4);;
  if(mcc != calc_mcc) {
    cmd_send(CMD_ACK, 0, 0, 0, 0, 0);
    goto OUT;
  }

  // OK
  cmd_send(CMD_ACK, 1, 0, 0, (uint8_t*)&card, sizeof(legic_card_select_t));

OUT:
  switch_off();
}

void LegicRfReader(uint16_t offset, uint16_t len, uint8_t iv) {
  // configure ARM and FPGA
  init_reader(false);

  // establish shared secret and detect card type
  uint8_t card_type = setup_phase_reader(iv);
  if(init_card(card_type, &card) != 0) {
    cmd_send(CMD_ACK, 0, 0, 0, 0, 0);
    goto OUT;
  }

  // do not read beyond card memory
  if(len + offset > card.cardsize) {
    len = card.cardsize - offset;
  }

  for(uint16_t i = 0; i < len; ++i) {
    int16_t byte = read_byte(offset + i, card.cmdsize);
    if(byte == -1) {
      cmd_send(CMD_ACK, 0, 0, 0, 0, 0);
      goto OUT;
    }
    legic_mem[i] = byte;
  }

  // OK
  cmd_send(CMD_ACK, 1, len, 0, legic_mem, len);

OUT:
  switch_off();
}

void LegicRfWriter(uint16_t offset, uint16_t len, uint8_t iv, uint8_t *data) {
  cmd_send(CMD_ACK, 0, 0, 0, 0, 0); //TODO Implement
}

void LegicRfSimulate(int phase, int frame, int reqresp) {
  cmd_send(CMD_ACK, 0, 0, 0, 0, 0); //TODO Implement
}
