#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdlib.h>
#include <avr/boot.h>
#include <util/delay.h>
#include <avr/pgmspace.h>

#define ARDUINO_DEBUG_SERIAL

#ifdef ARDUINO_DEBUG_SERIAL
#include <SendOnlySoftwareSerial.h>
SendOnlySoftwareSerial mySerial (PB2);  // Tx pin
#endif

#define LEDPORT (1<<PB1); //PB1 pin 6 Attiny85
#define INITLED {DDRB|=LEDPORT;}

#define LEDON {PORTB|=LEDPORT;}
#define LEDOFF {PORTB&=~LEDPORT;}
#define TOGGLELED {PORTB^=LEDPORT;}

#define INPUTAUDIOPIN (1<<PB0) //PB0 pin 5 attiny85
#define PINVALUE (PINB&INPUTAUDIOPIN)

#define INITPORT {PORTB|=INPUTAUDIOPIN;} //turn on pull up 

#define PINLOW (PINVALUE==0)
#define PINHIGH (!PINLOW)

#define WAITBLINKTIME 10000
#define BOOT_TIMEOUT 10

#define true (1==1)
#define false !true

//***************************************************************************************
// main loop
//***************************************************************************************

#define TIMER TCNT0 // we use timer1 for measuring time

// frame format definition
#define COMMAND         0
#define PAGEINDEXLOW    1  // page address lower part
#define PAGEINDEXHIGH   2  // page address higher part
#define LENGTHLOW       3
#define LENGTHHIGH      4
#define CRCLOW          5  // checksum lower part 
#define CRCHIGH         6  // checksum higher part 
#define DATAPAGESTART   7  // start of data
#define PAGESIZE    SPM_PAGESIZE
#define FRAMESIZE   (PAGESIZE+DATAPAGESTART) // size of the data block to be received

// bootloader commands
#define NOCOMMAND       0
#define TESTCOMMAND     1
#define PROGCOMMAND     2
#define RUNCOMMAND      3

uint8_t FrameData[FRAMESIZE];

// #define RJMP (0xC000U - 1) // opcode of RJMP minus offset 1
// #define RESET_SECTION __attribute __ ((section ("bootreset")))

// Microcontroller vectortable entries in the flash
#define RESET_VECTOR_OFFSET         0
// number of bytes before the boot loader vectors to store the tiny application vector table
#define TINYVECTOR_RESET_OFFSET     4
#define TINYVECTOR_OSCCAL_OFFSET    6
/* ------------------------------------------------------------------------ */
// postscript are the few bytes at the end of programmable memory which store tinyVectors
#define POSTSCRIPT_SIZE 6
#define PROGMEM_SIZE (BOOTLOADER_ADDRESS - POSTSCRIPT_SIZE) /* max size of user program */

#define FLASH_RESET_ADDR  0x0000                      // address of reset vector (in bytes)
#define BOOTLOADER_STARTADDRESS BOOTLOADER_ADDRESS    // start address:
#define BOOTLOADER_ENDADDRESS   0x2000                // end address:   0x2000 = 8192
#define LAST_PAGE (BOOTLOADER_STARTADDRESS - SPM_PAGESIZE)/SPM_PAGESIZE

#define RJMP    (0xC000U - 1)

#include <avr/boot.h>
#ifndef RWWSRE                                                                  // bug in AVR libc:
#define RWWSRE CTPB                                                             // RWWSRE is not defined on ATTinys, use CTBP instead
#endif

typedef union {
uint16_t w;
uint8_t b[2];
} uint16_union_t;

register uint16_union_t currentAddress  asm("r4");  // r4/r5 current progmem address, used for erasing and writing 

void (*start_appl_main) (void);

#define BOOTLOADER_FUNC_ADDRESS (BOOTLOADER_STARTADDRESS - sizeof (start_appl_main))


uint16_t saved_reset_vector;
uint8_t prog_count = 0;

//***************************************************************************************
// receiveFrame()
//
// This routine receives a differential manchester coded signal at the input pin.
// The routine waits for a toggling voltage level.
// It automatically detects the transmission speed.
//
// output:    uint8_t flag: true: checksum ok
//        Data // global variable
//
//***************************************************************************************
uint8_t receiveFrame()
{
  uint16_t store[16];

  uint16_t counter = 0;
  volatile uint16_t time = 0;
  volatile uint16_t delayTime;
  uint8_t p, t;
  uint8_t k = 8;
  uint8_t dataPointer = 0;
  uint16_t n;

  //*** synchronisation and bit rate estimation **************************
  time = 0;
  // wait for edge
  p = PINVALUE;
  while (p == PINVALUE);

  p = PINVALUE;

  TIMER = 0; // reset timer
  for (n = 0; n < 16; n++)
  {
    // wait for edge
    while (p == PINVALUE);
    t = TIMER;
    TIMER = 0; // reset timer
    p = PINVALUE;

    store[counter++] = t;

    if (n >= 8)time += t; // time accumulator for mean period calculation only the last 8 times are used
  }

  delayTime = time * 3 / 4 / 8;
  // delay 3/4 bit
  while (TIMER < delayTime);

  //p=1;

  //****************** wait for start bit ***************************
  while (p == PINVALUE) // while not startbit ( no change of pinValue means 0 bit )
  {
    // wait for edge
    while (p == PINVALUE);
    p = PINVALUE;
    TIMER = 0;

    // delay 3/4 bit
    while (TIMER < delayTime);
    TIMER = 0;

    counter++;
  }
  p = PINVALUE;
  //****************************************************************
  //receive data bits
  k = 8;
  for (n = 0; n < (FRAMESIZE * 8); n++)
  {
    // wait for edge
    while (p == PINVALUE);
    TIMER = 0;
    p = PINVALUE;

    // delay 3/4 bit
    while (TIMER < delayTime);

    t = PINVALUE;

    counter++;

    FrameData[dataPointer] = FrameData[dataPointer] << 1;
    if (p != t) FrameData[dataPointer] |= 1;
    p = t;
    k--;
    if (k == 0) {
      dataPointer++;
      k = 8;
    };
  }
  uint16_t crc = (uint16_t)FrameData[CRCLOW] + FrameData[CRCHIGH] * 256;
  return true;
  //else return false;
}

static inline void eraseApplication(void) {
  // uint16_t ptr = BOOTLOADER_ADDRESS;
  // while (ptr) {
  //   ptr -= SPM_PAGESIZE;        
  //   boot_page_erase(ptr);
  //   boot_spm_busy_wait ();      // Wait until the memory is erased.
  // }
}


/*-----------------------------------------------------------------------------------------------------------------------
 * Flash: fill page word by word
 *-----------------------------------------------------------------------------------------------------------------------
 */
#define boot_program_page_fill(byteaddr, word)      \
{                                                   \
    uint8_t sreg;                                   \
    sreg = SREG;                                    \
    cli ();                                         \
    boot_page_fill ((uint32_t) (byteaddr), word);   \
    SREG = sreg;                                    \
}

/*-----------------------------------------------------------------------------------------------------------------------
 * Flash: erase and write page
 *-----------------------------------------------------------------------------------------------------------------------
 */
#define boot_program_page_erase_write(pageaddr)     \
{                                                   \
    uint8_t sreg;                                   \
    eeprom_busy_wait ();                            \
    sreg = SREG;                                    \
    cli ();                                         \
    boot_page_erase ((uint32_t) (pageaddr));        \
    boot_spm_busy_wait ();                          \
    boot_page_write ((uint32_t) (pageaddr));        \
    boot_spm_busy_wait ();                          \
    boot_rww_enable ();                             \
    SREG = sreg;                                    \
}


/*-----------------------------------------------------------------------------------------------------------------------
 * write a block into flash
 *-----------------------------------------------------------------------------------------------------------------------
 */
static void
pgm_write_block (uint16_t flash_addr, uint16_t * block, size_t size)
{
    uint16_t        start_addr;
    uint16_t        addr;
    uint16_t        w;
    uint8_t         idx = 0;

    start_addr = (flash_addr / SPM_PAGESIZE) * SPM_PAGESIZE;        // round down (granularity is SPM_PAGESIZE)

    for (idx = 0; idx < SPM_PAGESIZE / 2; idx++)
    {
        addr = start_addr + 2 * idx;

        if (addr >= flash_addr && size > 0)
        {
            w = *block++;
            size -= sizeof (uint16_t);
        }
        else
        {
            w = pgm_read_word (addr);
        }

        boot_program_page_fill (addr, w);
    }

    boot_program_page_erase_write(start_addr);                      // erase and write the page
}


//***************************************************************************************
//  void boot_program_page (uint32_t page, uint8_t *buf)
//
//  Erase and flash one page.
//
//  inputt:     page address and data to be programmed
//
//***************************************************************************************
void boot_program_page (uint32_t page, uint8_t *buf)
{
  uint16_t i;
  cli(); // disable interrupts

    boot_page_erase(page);
    boot_spm_busy_wait ();      // Wait until the memory is erased.


  for (i = 0; i < SPM_PAGESIZE; i += 2)
  {
    //read received data
    uint16_t w = *buf++; //low section
    w += (*buf++) << 8; //high section
    //combine low and high to get 16 bit

    //first page and first index is vector table... ( page 0 and index 0 )
    if (page == 0 && i == 0)
    {void (*foo)(void *);
        //1.save jump to application vector for later patching      
        void* appl = (void *)(w - RJMP);
         start_appl_main=  ((void (*)(void)) appl);


        //2.replace w with jump vector to bootloader        
        w = 0xC000 + (BOOTLOADER_ADDRESS/2) - 1;
    }
    else if (page == LAST_PAGE && i == 60)
    {
      //3.retrieve saved reset vector
      w = saved_reset_vector;
    }

    boot_page_fill (page + i, w);
    boot_spm_busy_wait();       // Wait until the memory is written.
    currentAddress.w += 2;
  }

  boot_page_write (page);     // Store buffer in flash page.
  boot_spm_busy_wait();       // Wait until the memory is written.

}
//***************************************************************************************
void initstart()
{
  // Timer 2 normal mode, clk/8, count up from 0 to 255
  // ==> frequency @16MHz= 16MHz/8/256=7812.5Hz
  TCCR0B = _BV(CS01);
}
//***************************************************************************************


void runProgramm(void)
{
  // reintialize registers to default
  DDRB = 0;
  cli();
  TCCR0B = 0; // turn off timer1

  pgm_write_block (BOOTLOADER_FUNC_ADDRESS, (uint16_t *) &start_appl_main, sizeof (start_appl_main));


  start_appl_main();
  //asm volatile ("rjmp __vectors - 4"); // jump to application reset vector at end of flash
}

//***************************************************************************************
// main loop
//***************************************************************************************
void a_main()
{
  initstart();
  uint8_t p;
  uint16_t time = WAITBLINKTIME;
  uint8_t timeout = BOOT_TIMEOUT;




  //*************** wait for toggling input pin or timeout ******************************
  uint8_t exitcounter = 3;
  while (1)
  {

    if (TIMER > 100) // timedelay ==> frequency @16MHz= 16MHz/8/100=20kHz
    {
      TIMER = 0;
      time--;
      if (time == 0)
      {
        TOGGLELED;

#ifdef ARDUINO_DEBUG_SERIAL
        mySerial.println ("BIP!!!!");
#endif

        time = WAITBLINKTIME;
                timeout--;
                if (timeout == 0)
                {
                  LEDOFF; // timeout,
                  // leave bootloader and run program
                  
        memcpy_P (&start_appl_main, (PGM_P) BOOTLOADER_FUNC_ADDRESS, sizeof (start_appl_main));

        if (start_appl_main)
        {
            cli ();
            (*start_appl_main) ();
        }

                    runProgramm();
                }
      }
    }
    if (p != PINVALUE)
    {
      p = PINVALUE;
      exitcounter--;
    }
    if (exitcounter == 0) break; // signal received, leave this loop and go on
  }
  //*************** start command interpreter *************************************
  LEDON;
  while (1)
  {
    if (!receiveFrame())
    {
      //*****  error: blink fast, press reset to restart *******************

      while (1)
      {
        if (TIMER > 100) // timerstop ==> frequency @16MHz= 16MHz/8/100=20kHz
        {
          TIMER = 0;
          time--;
          if (time == 0)
          {
            TOGGLELED;
            time = 1000;
          }
        }
      }
    }
    else // succeed
    {
      #ifdef ARDUINO_DEBUG_SERIAL
  // mySerial.println(FrameData[COMMAND]);
#endif


      switch (FrameData[COMMAND])
      {

        case TESTCOMMAND: // not used yet
          {

          }
          break;
        case RUNCOMMAND:
          {


#ifdef ARDUINO_DEBUG_SERIAL
            mySerial.println("RUN");
#endif
            // leave bootloader and run program
            runProgramm();
          }
          break;
        case PROGCOMMAND:
          {
            uint16_t k = (((uint16_t)FrameData[PAGEINDEXHIGH]) << 8) + FrameData[PAGEINDEXLOW];

            mySerial.println(currentAddress.w);

            if(prog_count == 0) eraseApplication();
            
            prog_count++;

            boot_program_page (SPM_PAGESIZE * k, FrameData + DATAPAGESTART);  // erase and programm page

#ifdef ARDUINO_DEBUG_SERIAL
#endif            
          }
          break;
      }
      FrameData[COMMAND] = NOCOMMAND; // delete command
    }
  }
}

int main()
{
  INITLED;
#ifdef ARDUINO_DEBUG_SERIAL
  mySerial.begin(9600);
#endif
  a_main(); // start the main function
}
