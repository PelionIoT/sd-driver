/* mbed Microcontroller Library
 * Copyright (c) 2006-2012 ARM Limited
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef MBED_SD_BLOCK_DEVICE_H
#define MBED_SD_BLOCK_DEVICE_H

/* If the target has no SPI support then SDCard is not supported */
#ifdef DEVICE_SPI

#include "BlockDevice.h"
#include "mbed.h"

/* Fake Mutex object that'll be used if the RTOS is not being compiled */
#ifndef MBED_CONF_RTOS_PRESENT
class Mutex {
public:
    void lock() {}
    void unlock() {}
};
#endif // #ifndef MBED_CONF_RTOS_PRESENT


/** Access an SD Card using SPI
 *
 * @code
 * #include "mbed.h"
 * #include "SDBlockDevice.h"
 *
 * SDBlockDevice sd(p5, p6, p7, p12); // mosi, miso, sclk, cs
 * uint8_t block[512] = "Hello World!\n";
 *
 * int main() {
 *     sd.init();
 *     sd.write(block, 0, 512);
 *     sd.read(block, 0, 512);
 *     printf("%s", block);
 *     sd.deinit();
 * }
 */
class SDBlockDevice : public BlockDevice {
public:
    /** Lifetime of an SD card
     */
    SDBlockDevice(PinName mosi, PinName miso, PinName sclk, PinName cs, uint64_t hz=1000000);
    virtual ~SDBlockDevice();

    /** Initialize a block device
     *
     *  @return         0 on success or a negative error code on failure
     */
    virtual int init();

    /** Deinitialize a block device
     *
     *  @return         0 on success or a negative error code on failure
     */
    virtual int deinit();

    /** Read blocks from a block device
     *
     *  @param buffer   Buffer to write blocks to
     *  @param addr     Address of block to begin reading from
     *  @param size     Size to read in bytes, must be a multiple of read block size
     *  @return         0 on success, negative error code on failure
     */
    virtual int read(void *buffer, bd_addr_t addr, bd_size_t size);

    /** Program blocks to a block device
     *
     *  The blocks must have been erased prior to being programmed
     *
     *  @param buffer   Buffer of data to write to blocks
     *  @param addr     Address of block to begin writing to
     *  @param size     Size to write in bytes, must be a multiple of program block size
     *  @return         0 on success, negative error code on failure
     */
    virtual int program(const void *buffer, bd_addr_t addr, bd_size_t size);

    /** Mark blocks as no longer in use
     *
     *  This function provides a hint to the underlying block device that a region of blocks
     *  is no longer in use and may be erased without side effects. Erase must still be called
     *  before programming, but trimming allows flash-translation-layers to schedule erases when
     *  the device is not busy.
     *
     *  @param addr     Address of block to mark as unused
     *  @param size     Size to mark as unused in bytes, must be a multiple of erase block size
     *  @return         0 on success, negative error code on failure
     */
    virtual int trim(bd_addr_t addr, bd_size_t size);

    /** Get the size of a readable block
     *
     *  @return         Size of a readable block in bytes
     */
    virtual bd_size_t get_read_size() const;

    /** Get the size of a programable block
     *
     *  @return         Size of a programable block in bytes
     *  @note Must be a multiple of the read size
     */
    virtual bd_size_t get_program_size() const;

    /** Get the total size of the underlying device
     *
     *  @return         Size of the underlying device in bytes
     */
    virtual bd_size_t size() const;

    /** Enable or disable debugging
     *
     *  @param          State of debugging
     */
    virtual void debug(bool dbg);

    /** Set the transfer frequency
     *
     *  @param         Transfer frequency
     *  @note Max frequency supported is 25MHZ
     */
    virtual int frequency(uint64_t freq);


private:
    /* Commands : Listed below are commands supported
     * in SPI mode for SD card : Only Mandatory ones
     */
    enum cmdSupported {
        CMD_NOT_SUPPORTED = -1,             /**< Command not supported error */
        CMD0_GO_IDLE_STATE = 0,             /**< Resets the SD Memory Card */
        CMD1_SEND_OP_COND = 1,              /**< Sends host capacity support */
        CMD6_SWITCH_FUNC = 6,               /**< Check and Switches card function */
        CMD8_SEND_IF_COND = 8,              /**< Supply voltage info */
        CMD9_SEND_CSD = 9,                  /**< Provides Card Specific data */
        CMD10_SEND_CID = 10,                /**< Provides Card Identification */
        CMD12_STOP_TRANSMISSION = 12,       /**< Forces the card to stop transmission */
        CMD13_SEND_STATUS = 13,             /**< Card responds with status */
        CMD16_SET_BLOCKLEN = 16,            /**< Length for SC card is set */
        CMD17_READ_SINGLE_BLOCK = 17,       /**< Read single block of data */
        CMD18_READ_MULTIPLE_BLOCK = 18,     /**< Card transfers data blocks to host until interrupted
                                                 by a STOP_TRANSMISSION command */
        CMD24_WRITE_BLOCK = 24,             /**< Write single block of data */
        CMD25_WRITE_MULTIPLE_BLOCK = 25,    /**< Continuously writes blocks of data until
                                                 'Stop Tran' token is sent */
        CMD27_PROGRAM_CSD = 27,             /**< Programming bits of CSD */
        CMD32_ERASE_WR_BLK_START_ADDR = 32, /**< Sets the address of the first write
                                                 block to be erased. */
        CMD33_ERASE_WR_BLK_END_ADDR = 33,   /**< Sets the address of the last write
                                                 block of the continuous range to be erased.*/
        CMD38_ERASE = 38,                   /**< Erases all previously selected write blocks */
        CMD55_APP_CMD = 55,                 /**< Extend to Applications specific commands */
        CMD56_GEN_CMD = 56,                 /**< General Purpose Command */
        CMD58_READ_OCR = 58,                /**< Read OCR register of card */
        CMD59_CRC_ON_OFF = 59,              /**< Turns the CRC option on or off*/
        // App Commands
        ACMD6_SET_BUS_WIDTH = 6,
        ACMD13_SD_STATUS = 13,
        ACMD22_SEND_NUM_WR_BLOCKS = 22,
        ACMD23_SET_WR_BLK_ERASE_COUNT = 23,
        ACMD41_SD_SEND_OP_COND = 41,
        ACMD42_SET_CLR_CARD_DETECT = 42,
        ACMD51_SEND_SCR = 51,
    };

    uint8_t _card_type;
    int _cmd(SDBlockDevice::cmdSupported cmd, uint32_t arg, bool isAcmd=0, uint32_t *resp=NULL);
    int _cmd8();

    /*  Move the SDCard into the SPI Mode idle state
     *
     *  The card is transitioned from SDCard mode to SPI mode by sending the
     *  CMD0 (GO_IDLE_STATE) command with CS asserted. See the notes in the
     *  "SPI Startup" section of the comments at the head of the
     *  implementation file for further details and specification references.
     *
     *  @return         Response form the card. R1_IDLE_STATE (0x1), the successful
     *                  response from CMD0. R1_XXX_XXX for more response
     */
    uint32_t _go_idle_state();
    int _initialise_card();

    uint32_t _sectors;
    uint32_t _sd_sectors();

    bool _is_valid_trim(bd_addr_t addr, bd_size_t size);

    /* SPI functions */
    Timer _spi_timer;               /**< Timer Class object used for busy wait */
    uint32_t _init_sck;             /**< Intial SPI frequency */
    uint32_t _transfer_sck;         /**< SPI frequency during data transfer/after initialization */
    SPI _spi;                       /**< SPI Class object */

    /* SPI initialization function */
    void _spi_init();
    uint8_t _cmd_spi(SDBlockDevice::cmdSupported cmd, uint32_t arg);
    void _spi_wait(uint8_t count);

    bool _wait_token(uint8_t token);        /**< Wait for token */
    bool _wait_ready(uint16_t ms=300);      /**< 300ms default wait for card to be ready */
    int _read(uint8_t * buffer, uint32_t length);
    int _read_bytes(uint8_t * buffer, uint32_t length);
    uint8_t _write(const uint8_t *buffer,uint8_t token, uint32_t length);
    int _freq(void);

    /* Chip Select and SPI mode select */
    DigitalOut _cs;
    void _select();
    void _deselect();

    mutable Mutex _lock;
    uint32_t _block_size;
    uint32_t _erase_size;
    bool _is_initialized;
    bool _dbg;
};

#endif  /* DEVICE_SPI */

#endif  /* MBED_SD_BLOCK_DEVICE_H */
