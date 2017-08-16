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
/* Introduction
 * ------------
 * SD and MMC cards support a number of interfaces, but common to them all
 * is one based on SPI. Since we already have the mbed SPI Interface, it will
 * be used for SD cards.
 *
 * The main reference I'm using is Chapter 7, "SPI Mode" of:
 *  http://www.sdcard.org/developers/tech/sdcard/pls/Simplified_Physical_Layer_Spec.pdf
 *
 * SPI Startup
 * -----------
 * The SD card powers up in SD mode. The start-up procedure is complicated
 * by the requirement to support older SDCards in a backwards compatible
 * way with the new higher capacity variants SDHC and SDHC.
 *
 * The following figures from the specification with associated text describe
 * the SPI mode initialisation process:
 *  - Figure 7-1: SD Memory Card State Diagram (SPI mode)
 *  - Figure 7-2: SPI Mode Initialization Flow
 *
 * Firstly, a low initial clock should be selected (in the range of 100-
 * 400kHZ). After initialisation has been completed, the switch to a
 * higher clock speed can be made (e.g. 1MHz). Newer cards will support
 * higher speeds than the default _transfer_sck defined here.
 *
 * Next, note the following from the SDCard specification (note to
 * Figure 7-1):
 *
 *  In any of the cases CMD1 is not recommended because it may be difficult for the host
 *  to distinguish between MultiMediaCard and SD Memory Card
 *
 * Hence CMD1 is not used for the initialisation sequence.
 *
 * The SPI interface mode is selected by asserting CS low and sending the
 * reset command (CMD0). The card will respond with a (R1) response.
 * In practice many cards initially respond with 0xff or invalid data
 * which is ignored. Data is read until a valid response is received
 * or the number of re-reads has exceeded a maximim count. If a valid
 * response is not received then the CMD0 can be retried. This
 * has been found to successfully initialise cards where the SPI master
 * (on MCU) has been reset but the SDCard has not, so the first
 * CMD0 may be lost.
 *
 * CMD8 is optionally sent to determine the voltage range supported, and
 * indirectly determine whether it is a version 1.x SD/non-SD card or
 * version 2.x. I'll just ignore this for now.
 *
 * ACMD41 is repeatedly issued to initialise the card, until "in idle"
 * (bit 0) of the R1 response goes to '0', indicating it is initialised.
 *
 * You should also indicate whether the host supports High Capicity cards,
 * and check whether the card is high capacity - i'll also ignore this
 *
 * SPI Protocol
 * ------------
 * The SD SPI protocol is based on transactions made up of 8-bit words, with
 * the host starting every bus transaction by asserting the CS signal low. The
 * card always responds to commands, data blocks and errors.
 *
 * The protocol supports a CRC, but by default it is off (except for the
 * first reset CMD0, where the CRC can just be pre-calculated, and CMD8)
 * I'll leave the CRC off I think!
 *
 * Standard capacity cards have variable data block sizes, whereas High
 * Capacity cards fix the size of data block to 512 bytes. I'll therefore
 * just always use the Standard Capacity cards with a block size of 512 bytes.
 * This is set with CMD16.
 *
 * You can read and write single blocks (CMD17, CMD25) or multiple blocks
 * (CMD18, CMD25). For simplicity, I'll just use single block accesses. When
 * the card gets a read command, it responds with a response token, and then
 * a data token or an error.
 *
 * SPI Command Format
 * ------------------
 * Commands are 6-bytes long, containing the command, 32-bit argument, and CRC.
 *
 * +---------------+------------+------------+-----------+----------+--------------+
 * | 01 | cmd[5:0] | arg[31:24] | arg[23:16] | arg[15:8] | arg[7:0] | crc[6:0] | 1 |
 * +---------------+------------+------------+-----------+----------+--------------+
 *
 * As I'm not using CRC, I can fix that byte to what is needed for CMD0 (0x95)
 *
 * All Application Specific commands shall be preceded with APP_CMD (CMD55).
 *
 * SPI Response Format
 * -------------------
 * The main response format (R1) is a status byte (normally zero). Key flags:
 *  idle - 1 if the card is in an idle state/initialising
 *  cmd  - 1 if an illegal command code was detected
 *
 *    +-------------------------------------------------+
 * R1 | 0 | arg | addr | seq | crc | cmd | erase | idle |
 *    +-------------------------------------------------+
 *
 * R1b is the same, except it is followed by a busy signal (zeros) until
 * the first non-zero byte when it is ready again.
 *
 * Data Response Token
 * -------------------
 * Every data block written to the card is acknowledged by a byte
 * response token
 *
 * +----------------------+
 * | xxx | 0 | status | 1 |
 * +----------------------+
 *              010 - OK!
 *              101 - CRC Error
 *              110 - Write Error
 *
 * Single Block Read and Write
 * ---------------------------
 *
 * Block transfers have a byte header, followed by the data, followed
 * by a 16-bit CRC. In our case, the data will always be 512 bytes.
 *
 * +------+---------+---------+- -  - -+---------+-----------+----------+
 * | 0xFE | data[0] | data[1] |        | data[n] | crc[15:8] | crc[7:0] |
 * +------+---------+---------+- -  - -+---------+-----------+----------+
 */

/* If the target has no SPI support then SDCard is not supported */
#ifdef DEVICE_SPI

#include "SDBlockDevice.h"
#include "mbed_debug.h"
#include <errno.h>

/* Required version: 5.5.4 and above */
#if (MBED_VERSION < MBED_ENCODE_VERSION(5,5,4))
#error "Incompatible mbed-os version detected! Required 5.5.4 and above"
#endif

#define SD_COMMAND_TIMEOUT                       5000   /*!< Timeout in ms for response */
#define SD_CMD0_GO_IDLE_STATE_RETRIES            5      /*!< Number of retries for sending CMDO */
#define SD_DBG                                   0      /*!< 1 - Enable debugging */
#define SD_CMD_TRACE                             0      /*!< 1 - Enable SD command tracing */

#define SD_BLOCK_DEVICE_ERROR_WOULD_BLOCK        -5001	/*!< operation would block */
#define SD_BLOCK_DEVICE_ERROR_UNSUPPORTED        -5002	/*!< unsupported operation */
#define SD_BLOCK_DEVICE_ERROR_PARAMETER          -5003	/*!< invalid parameter */
#define SD_BLOCK_DEVICE_ERROR_NO_INIT            -5004	/*!< uninitialized */
#define SD_BLOCK_DEVICE_ERROR_NO_DEVICE          -5005	/*!< device is missing or not connected */
#define SD_BLOCK_DEVICE_ERROR_WRITE_PROTECTED    -5006	/*!< write protected */
#define SD_BLOCK_DEVICE_ERROR_UNUSABLE           -5007  /*!< unusable card */
#define SD_BLOCK_DEVICE_ERROR_NO_RESPONSE        -5008  /*!< No response from device */
#define SD_BLOCK_DEVICE_ERROR_CRC                -5009  /*!< CRC error */
#define SD_BLOCK_DEVICE_ERROR_ERASE              -5010  /*!< Erase error: reset/sequence */
#define SD_BLOCK_DEVICE_ERROR_WRITE              -5011  /*!< SPI Write error: !SPI_DATA_ACCEPTED */

#define BLOCK_SIZE_HC                            512    /*!< Block size supported for SD card is 512 bytes  */
#define WRITE_BL_PARTIAL                         0      /*!< Partial block write - Not supported */
#define CRC_SUPPORT                              0      /*!< CRC - Not supported */
#define SPI_CMD(x) (0x40 | (x & 0x3f))

/* R1 Response Format */
#define R1_NO_RESPONSE          (0xFF)
#define R1_RESPONSE_RECV        (0x80)
#define R1_IDLE_STATE           (1 << 0)
#define R1_ERASE_RESET          (1 << 1)
#define R1_ILLEGAL_COMMAND      (1 << 2)
#define R1_COM_CRC_ERROR        (1 << 3)
#define R1_ERASE_SEQUENCE_ERROR (1 << 4)
#define R1_ADDRESS_ERROR        (1 << 5)
#define R1_PARAMETER_ERROR      (1 << 6)

// Types
#define SDCARD_NONE              0           /**< No card is present */
#define SDCARD_V1                1           /**< v1.x Standard Capacity */
#define SDCARD_V2                2           /**< v2.x Standard capacity SD card */
#define SDCARD_V2HC              3           /**< v2.x High capacity SD card */
#define CARD_UNKNOWN             4           /**< Unknown or unsupported card */

/* SIZE in Bytes */
#define PACKET_SIZE              6           /*!< SD Packet size CMD+ARG+CRC */
#define R1_RESPONSE_SIZE         1           /*!< Size of R1 response */
#define R2_RESPONSE_SIZE         2           /*!< Size of R2 response */
#define R3_R7_RESPONSE_SIZE      5           /*!< Size of R3/R7 response */

/* R1b Response */
#define DEVICE_BUSY             (0x00)

/* R2 Response Format */
#define R2_CARD_LOCKED          (1 << 0)
#define R2_CMD_FAILED           (1 << 1)
#define R2_ERROR                (1 << 2)
#define R2_CC_ERROR             (1 << 3)
#define R2_CC_FAILED            (1 << 4)
#define R2_WP_VIOLATION         (1 << 5)
#define R2_ERASE_PARAM          (1 << 6)
#define R2_OUT_OF_RANGE         (1 << 7)

/* R3 Response : OCR Register */
#define OCR_HCS_CCS             (0x1 << 30)
#define OCR_LOW_VOLTAGE         (0x01 << 24)
#define OCR_3_3V                (0x1 << 20)

/* R7 response pattern for CMD8 */
#define CMD8_PATTERN             (0xAA)

/*  CRC Enable  */
#define CRC_ENABLE               (0)         /*!< CRC 1 - Enable 0 - Disable */

/* Control Tokens   */
#define SPI_DATA_RESPONSE_MASK   (0x1F)
#define SPI_DATA_ACCEPTED        (0x05)
#define SPI_DATA_CRC_ERROR       (0x0B)
#define SPI_DATA_WRITE_ERROR     (0x0D)
#define SPI_START_BLOCK          (0xFE)      /*!< For Single Block Read/Write and Multiple Block Read */
#define SPI_START_BLK_MUL_WRITE  (0xFC)      /*!< Start Multi-block write */
#define SPI_STOP_TRAN            (0xFD)      /*!< Stop Multi-block write */

#define SPI_DATA_READ_ERROR_MASK (0xF)       /*!< Data Error Token: 4 LSB bits */
#define SPI_READ_ERROR           (0x1 << 0)  /*!< Error */
#define SPI_READ_ERROR_CC        (0x1 << 1)  /*!< CC Error*/
#define SPI_READ_ERROR_ECC_C     (0x1 << 2)  /*!< Card ECC failed */
#define SPI_READ_ERROR_OFR       (0x1 << 3)  /*!< Out of Range */

SDBlockDevice::SDBlockDevice(PinName mosi, PinName miso, PinName sclk, PinName cs, uint64_t hz)
    : _spi(mosi, miso, sclk), _cs(cs), _is_initialized(0)
{
    _cs = 1;
    _card_type = SDCARD_NONE;

    // Set default to 100kHz for initialisation and 1MHz for data transfer
    _init_sck = 100000;
    _transfer_sck = hz;

    // Only HC block size is supported.
    _block_size = BLOCK_SIZE_HC;
}

SDBlockDevice::~SDBlockDevice()
{
    if (_is_initialized) {
        deinit();
    }
}

int SDBlockDevice::_initialise_card()
{
    // Detail debugging is for commands
    _dbg = SD_DBG ? SD_CMD_TRACE : 0;
    int32_t status = BD_ERROR_OK;
    uint32_t response, arg;

    // Initialize the SPI interface: Card by default is in SD mode
    _spi_init();

    // The card is transitioned from SDCard mode to SPI mode by sending the CMD0 + CS Asserted("0")
    if (_go_idle_state() != R1_IDLE_STATE) {
        debug_if(SD_DBG, "No disk, or could not put SD card in to SPI idle state\n");
        return SD_BLOCK_DEVICE_ERROR_NO_DEVICE;
    }

    // Send CMD8
    if (BD_ERROR_OK != (status = _cmd8())) {
        return status;
    }

    // Disable CRC
    status = _cmd(CMD59_CRC_ON_OFF, 0);

    // Read OCR - CMD58 Response contains OCR register
    if (BD_ERROR_OK != (status = _cmd(CMD58_READ_OCR, 0x0, 0x0, &response))) {
        return status;
    }

    // Check if card supports voltage range: 3.3V
    if (!(response & OCR_3_3V)) {
        _card_type = CARD_UNKNOWN;
        status = SD_BLOCK_DEVICE_ERROR_UNUSABLE;
        return status;
    }

    // HCS is set 1 for HC/XC capacity cards for ACMD41, if supported
    arg = 0x0;
    if (SDCARD_V2 == _card_type) {
        arg |= OCR_HCS_CCS;
    }

    /* Idle state bit in the R1 response of ACMD41 is used by the card to inform the host
     * if initialization of ACMD41 is completed. "1" indicates that the card is still initializing.
     * "0" indicates completion of initialization. The host repeatedly issues ACMD41 until
     * this bit is set to "0".
     */
    _spi_timer.start();
    do {
        status = _cmd(ACMD41_SD_SEND_OP_COND, arg, 1, &response);
    } while ((response & R1_IDLE_STATE) && (_spi_timer.read_ms() < SD_COMMAND_TIMEOUT));
    _spi_timer.stop();

    // Initialization complete: ACMD41 successful
    if ((BD_ERROR_OK != status) || (0x00 != response)) {
        _card_type = CARD_UNKNOWN;
        debug_if(SD_DBG, "Timeout waiting for card\n");
        return status;
    }

    if (SDCARD_V2 == _card_type) {
        // Get the card capacity CCS: CMD58
        if (BD_ERROR_OK == (status = _cmd(CMD58_READ_OCR, 0x0, 0x0, &response))) {
            // High Capacity card
            if (response & OCR_HCS_CCS) {
                _card_type = SDCARD_V2HC;
                debug_if(SD_DBG, "Card Initialized: High Capacity Card \n");
            } else {
                debug_if(SD_DBG, "Card Initialized: Standard Capacity Card: Version 2.x \n");
            }
        }
    } else {
        _card_type = SDCARD_V1;
        debug_if(SD_DBG, "Card Initialized: Version 1.x Card\n");
    }
    return status;
}


int SDBlockDevice::init()
{
    _lock.lock();
    int err = _initialise_card();
    _is_initialized = (err == BD_ERROR_OK);
    if (!_is_initialized) {
        debug_if(SD_DBG, "Fail to initialize card\n");
        _lock.unlock();
        return err;
    }
    debug_if(SD_DBG, "init card = %d\n", _is_initialized);
    _sectors = _sd_sectors();
    // CMD9 failed
    if (0 == _sectors) {
        _lock.unlock();
        return BD_ERROR_DEVICE_ERROR;
    }

    // Set block length to 512 (CMD16)
    if (_cmd(CMD16_SET_BLOCKLEN, _block_size) != 0) {
        debug_if(SD_DBG, "Set %d-byte block timed out\n", _block_size);
        _lock.unlock();
        return BD_ERROR_DEVICE_ERROR;
    }

    // Set SCK for data transfer
    err = _freq();
    if (err) {
        _lock.unlock();
        return err;
    }
    _lock.unlock();
    return BD_ERROR_OK;
}

int SDBlockDevice::deinit()
{
    _lock.lock();
    _is_initialized = false;
    _lock.unlock();
    return 0;
}


int SDBlockDevice::program(const void *b, bd_addr_t addr, bd_size_t size)
{
    if (!is_valid_program(addr, size)) {
        return SD_BLOCK_DEVICE_ERROR_PARAMETER;
    }

    _lock.lock();
    if (!_is_initialized) {
        _lock.unlock();
        return SD_BLOCK_DEVICE_ERROR_NO_INIT;
    }

    const uint8_t *buffer = static_cast<const uint8_t*>(b);
    int status = BD_ERROR_OK;
    uint8_t response;

    // Get block count
    bd_addr_t blockCnt = size / _block_size;

    // SDSC Card (CCS=0) uses byte unit address
    // SDHC and SDXC Cards (CCS=1) use block unit address (512 Bytes unit)
    if(SDCARD_V2HC == _card_type) {
        addr = addr / _block_size;
    }

    // Send command to perform write operation
    if (blockCnt == 1) {
        // Single block write command
        if (BD_ERROR_OK != (status = _cmd(CMD24_WRITE_BLOCK, addr))) {
            _lock.unlock();
            return status;
        }

        // Write data
        response = _write(buffer, SPI_START_BLOCK, _block_size);
        // Only CRC and general write error are communicated via response token
        if ((response == SPI_DATA_CRC_ERROR) || (response == SPI_DATA_WRITE_ERROR)) {
            debug_if(SD_DBG, "Single Block Write failed: 0x%x \n", response);
            status = SD_BLOCK_DEVICE_ERROR_WRITE;
        }

        /* Once the programming operation is completed, the host should check the
         * results of the programming using the SEND_STATUS command (CMD13).
         */
        status = _cmd(CMD13_SEND_STATUS, 0);
    } else {
        // Pre-erase setting prior to multiple block write operation
        _cmd(ACMD23_SET_WR_BLK_ERASE_COUNT, blockCnt, 1);
        // Multiple block write command
        if (BD_ERROR_OK != (status = _cmd(CMD25_WRITE_MULTIPLE_BLOCK, addr))) {
            _lock.unlock();
            return status;
        }

        // Write the data: one block at a time
        do {
            response = _write(buffer, SPI_START_BLK_MUL_WRITE, _block_size);
            if (response != SPI_DATA_ACCEPTED) {
                debug_if(SD_DBG, "Multiple Block Write failed: 0x%x \n", response);
                break;
            }
            buffer += _block_size;
        }while (--blockCnt);     // Receive all blocks of data

        /* In a Multiple Block write operation, the stop transmission will be done by
         * sending 'Stop Tran' token instead of 'Start Block' token at the beginning
         * of the next block
         */
        _select();
        _spi.write(SPI_STOP_TRAN);
        _deselect();

        // Wait for last block to be written
        if (false == _wait_ready(SD_COMMAND_TIMEOUT)) {
            debug_if(SD_DBG, "Card not ready yet \n");
        }

        /* In case of Write Error indication (on the data response) the host shall
         * use SEND_NUM_WR_BLOCKS (ACMD22) in order to get the number of well written write blocks.
         */
        if(response == SPI_DATA_WRITE_ERROR) {
            if (BD_ERROR_OK == _cmd(ACMD22_SEND_NUM_WR_BLOCKS, 0, 1)) {
                uint8_t wr_blocks[4];
                if (BD_ERROR_OK == _read_bytes(wr_blocks, 4)) {
                    debug_if(_dbg, "Blocks Written without errors: 0x%x\n",
                            ((wr_blocks[0] << 24) | (wr_blocks[1] << 16) | (wr_blocks[2] << 0) | wr_blocks[3]));
                }
            }
        }
    }
    _lock.unlock();
    return status;
}

int SDBlockDevice::read(void *b, bd_addr_t addr, bd_size_t size)
{
    if (!is_valid_read(addr, size)) {
        return SD_BLOCK_DEVICE_ERROR_PARAMETER;
    }

    _lock.lock();
    if (!_is_initialized) {
        _lock.unlock();
        return SD_BLOCK_DEVICE_ERROR_PARAMETER;
    }

    uint8_t *buffer = static_cast<uint8_t *>(b);
    int status = BD_ERROR_OK;
    bd_addr_t blockCnt =  size / _block_size;

    // SDSC Card (CCS=0) uses byte unit address
    // SDHC and SDXC Cards (CCS=1) use block unit address (512 Bytes unit)
    if (SDCARD_V2HC == _card_type) {
        addr = addr / _block_size;
    }

    for (uint8_t i = 0; i < 3; i++) {
        // Write command ro receive data
        if (blockCnt > 1) {
            status = _cmd(CMD18_READ_MULTIPLE_BLOCK, addr);
        } else {
            status = _cmd(CMD17_READ_SINGLE_BLOCK, addr);
        }
        if (BD_ERROR_OK != status) {
            _lock.unlock();
            return status;
        }

        // For the first time in read if start token is not received
        // means command is lost, retry sending read command once more
        if (SD_BLOCK_DEVICE_ERROR_NO_RESPONSE == _read(buffer, _block_size)) {
            continue;
        } else {
            buffer += _block_size;
            --blockCnt;
            break;
        }
    }

    // receive the data : one block at a time
    while (blockCnt) {
        if (0 != _read(buffer, _block_size)) {
            status = SD_BLOCK_DEVICE_ERROR_NO_RESPONSE;
            break;
        }
        buffer += _block_size;
        --blockCnt;
    }

    // Send CMD12(0x00000000) to stop the transmission for multi-block transfer
    if (size > _block_size) {
        status = _cmd(CMD12_STOP_TRANSMISSION, 0x0);
    }
    _lock.unlock();
    return status;
}

int SDBlockDevice::erase(bd_addr_t addr, bd_size_t size)
{
    if (!is_valid_erase(addr, size)) {
        return SD_BLOCK_DEVICE_ERROR_PARAMETER;
    }

    _lock.lock();
    if (!_is_initialized) {
        _lock.unlock();
        return SD_BLOCK_DEVICE_ERROR_NO_INIT;
    }
    int status = BD_ERROR_OK;

    size -= _block_size;
    // SDSC Card (CCS=0) uses byte unit address
    // SDHC and SDXC Cards (CCS=1) use block unit address (512 Bytes unit)
    if (SDCARD_V2HC == _card_type) {
        size = size / _block_size;
        addr = addr / _block_size;
    }

    // Start lba sent in start command
    if (BD_ERROR_OK != (status = _cmd(CMD32_ERASE_WR_BLK_START_ADDR, addr))) {
        _lock.unlock();
        return status;
    }

    // End lba = addr+size sent in end addr command
    if (BD_ERROR_OK != (status = _cmd(CMD33_ERASE_WR_BLK_END_ADDR, addr+size))) {
        _lock.unlock();
        return status;
    }
    status = _cmd(CMD38_ERASE, 0x0);
    _lock.unlock();
    return status;
}

bd_size_t SDBlockDevice::get_read_size() const
{
    return _block_size;
}

bd_size_t SDBlockDevice::get_program_size() const
{
    return _block_size;
}

bd_size_t SDBlockDevice::get_erase_size() const
{
    return _erase_size;
}

bd_size_t SDBlockDevice::size() const
{
    bd_size_t sectors = 0;
    _lock.lock();
    if (_is_initialized) {
    	sectors = _sectors;
    }
    _lock.unlock();
    return _block_size*sectors;
}

void SDBlockDevice::debug(bool dbg)
{
    _dbg = dbg;
}

int SDBlockDevice::frequency(uint64_t freq)
{
    _lock.lock();
    _transfer_sck = freq;
    int err = _freq();
    _lock.unlock();
    return err;
}

// PRIVATE FUNCTIONS
int SDBlockDevice::_freq(void)
{
    // Max frequency supported is 25MHZ
    if (_transfer_sck <= 25000000) {
        _spi.frequency(_transfer_sck);
        return 0;
    } else {  // TODO: Switch function to be implemented for higher frequency
        _transfer_sck = 25000000;
        _spi.frequency(_transfer_sck);
        return -EINVAL;
    }
}

uint8_t SDBlockDevice::_cmd_spi(SDBlockDevice::cmdSupported cmd, uint32_t arg) {
    uint8_t response;
    char cmdPacket[PACKET_SIZE];

    // Prepare the command packet
    cmdPacket[0] = SPI_CMD(cmd);
    cmdPacket[1] = (arg >> 24);
    cmdPacket[2] = (arg >> 16);
    cmdPacket[3] = (arg >> 8);
    cmdPacket[4] = (arg >> 0);
    // CMD0 is executed in SD mode, hence should have correct CRC
    // CMD8 CRC verification is always enabled
    switch(cmd) {
        case CMD0_GO_IDLE_STATE:
            cmdPacket[5] = 0x95;
            break;
        case CMD8_SEND_IF_COND:
            cmdPacket[5] = 0x87;
            break;
        default:
            cmdPacket[5] = 0xFF;    // Make sure bit 0-End bit is high
            break;
    }

    // send a command
    for (int i = 0; i < PACKET_SIZE; i++) {
        _spi.write(cmdPacket[i]);
    }

    // The received byte immediataly following CMD12 is a stuff byte,
    // it should be discarded before receive the response of the CMD12.
    if (CMD12_STOP_TRANSMISSION == cmd) {
        _spi.write(SPI_FILL_CHAR);
    }

    // Loop for response: Response is sent back within command response time (NCR), 0 to 8 bytes for SDC
    for (int i = 0; i < 0x10; i++) {
        response = _spi.write(SPI_FILL_CHAR);
        // Got the response
        if (!(response & R1_RESPONSE_RECV)) {
            break;
        }
    }
    return response;
}

int SDBlockDevice::_cmd(SDBlockDevice::cmdSupported cmd, uint32_t arg, bool isAcmd, uint32_t *resp) {
    int32_t status = BD_ERROR_OK;
    uint32_t response;

    // Select card and wait for card to be ready before sending next command
    // Note: next command will fail if card is not ready
    _select();
    if (false == _wait_ready(SD_COMMAND_TIMEOUT)) {
        debug_if(SD_DBG, "Card not ready yet \n");
    }

    // Re-try command
    for(int i = 0; i < 3; i++) {
        // Send CMD55 for APP command first
        if (isAcmd) {
            _cmd_spi(CMD55_APP_CMD, 0x0);
        }

        // Send command over SPI interface
        response = _cmd_spi(cmd, arg);
        if (R1_NO_RESPONSE == response) {
            debug_if(SD_DBG, "No response CMD:%d \n", cmd);
            continue;
        }
        break;
    }

    // Pass the response to the command call if required
    if (NULL != resp) {
        *resp = response;
    }

    // Process the response R1  : Exit on CRC/Illegal command error/No response
    if (R1_NO_RESPONSE == response) {
        _deselect();
        debug_if(SD_DBG, "No response CMD:%d \n", cmd);
        return SD_BLOCK_DEVICE_ERROR_NO_DEVICE;         // No device
    }
    if (response & R1_COM_CRC_ERROR) {
        _deselect();
        debug_if(SD_DBG, "CRC error CMD:%d \n", cmd);
        return SD_BLOCK_DEVICE_ERROR_CRC;                // CRC error
    }
    if (response & R1_ILLEGAL_COMMAND) {
        debug_if(SD_DBG, "Illegal command CMD:%d\n", cmd);
        if (CMD8_SEND_IF_COND == cmd) {                  // Illegal command is for Ver1 or not SD Card
            _card_type = CARD_UNKNOWN;
        }
        _deselect();
        return SD_BLOCK_DEVICE_ERROR_UNSUPPORTED;      // Command not supported
    }

    debug_if(_dbg, "CMD:%d \t arg:0x%x \t Response:0x%x \n", cmd, arg, response);
    // Set status for other errors
    if ((response & R1_ERASE_RESET) || (response & R1_ERASE_SEQUENCE_ERROR)) {
        status = SD_BLOCK_DEVICE_ERROR_ERASE;            // Erase error
    }else if ((response & R1_ADDRESS_ERROR) || (response & R1_PARAMETER_ERROR)) {
        // Misaligned address / invalid address block length
        status = SD_BLOCK_DEVICE_ERROR_PARAMETER;
    }

    // Get rest of the response part for other commands
    switch(cmd) {
        case CMD8_SEND_IF_COND:             // Response R7
            debug_if(_dbg, "V2-Version Card\n");
            _card_type = SDCARD_V2;
            // Note: No break here, need to read rest of the response
        case CMD58_READ_OCR:                // Response R3
            response  = (_spi.write(SPI_FILL_CHAR) << 24);
            response |= (_spi.write(SPI_FILL_CHAR) << 16);
            response |= (_spi.write(SPI_FILL_CHAR) << 8);
            response |= _spi.write(SPI_FILL_CHAR);
            debug_if(_dbg, "R3/R7: 0x%x \n", response);
            break;

        case CMD12_STOP_TRANSMISSION:       // Response R1b
        case CMD38_ERASE:
            _wait_ready(SD_COMMAND_TIMEOUT);
            break;

        case ACMD13_SD_STATUS:             // Response R2
            response = _spi.write(SPI_FILL_CHAR);
            debug_if(_dbg, "R2: 0x%x \n", response);
            break;

        default:                            // Response R1
            break;
    }

    // Pass the updated response to the command
    if (NULL != resp) {
        *resp = response;
    }

    // Deselect card
    _deselect();
    return status;
}

int SDBlockDevice::_cmd8() {
    uint32_t arg = (CMD8_PATTERN << 0);         // [7:0]check pattern
    uint32_t response = 0;
    int32_t status = BD_ERROR_OK;

    arg |= (0x1 << 8);  // 2.7-3.6V             // [11:8]supply voltage(VHS)

    status = _cmd(CMD8_SEND_IF_COND, arg, 0x0, &response);
    // Verify voltage and pattern for V2 version of card
    if ((BD_ERROR_OK == status) && (SDCARD_V2 == _card_type)) {
        // If check pattern is not matched, CMD8 communication is not valid
        if((response & 0xFFF) != arg)
        {
            debug_if(SD_DBG, "CMD8 Pattern mismatch 0x%x : 0x%x\n", arg, response);
            _card_type = CARD_UNKNOWN;
            status = SD_BLOCK_DEVICE_ERROR_UNUSABLE;
        }
    }
    return status;
}

uint32_t SDBlockDevice::_go_idle_state() {
    uint32_t response;

    /* Reseting the MCU SPI master may not reset the on-board SDCard, in which
     * case when MCU power-on occurs the SDCard will resume operations as
     * though there was no reset. In this scenario the first CMD0 will
     * not be interpreted as a command and get lost. For some cards retrying
     * the command overcomes this situation. */
    for (int i = 0; i < SD_CMD0_GO_IDLE_STATE_RETRIES; i++) {
        _cmd(CMD0_GO_IDLE_STATE, 0x0, 0x0, &response);
        if (R1_IDLE_STATE == response)
            break;
        wait_ms(1);
    }
    return response;
}

int SDBlockDevice::_read_bytes(uint8_t *buffer, uint32_t length) {
    uint16_t crc;

    _select();

    // read until start byte (0xFE)
    if (false == _wait_token(SPI_START_BLOCK)) {
        debug_if(SD_DBG, "Read timeout\n");
        _deselect();
        return SD_BLOCK_DEVICE_ERROR_NO_RESPONSE;
    }

    // read data
    for (uint32_t i = 0; i < length; i++) {
        buffer[i] = _spi.write(SPI_FILL_CHAR);
    }

    // Read the CRC16 checksum for the data block
    crc = (_spi.write(SPI_FILL_CHAR) << 8);
    crc |= _spi.write(SPI_FILL_CHAR);

    _deselect();
    return 0;
}

int SDBlockDevice::_read(uint8_t *buffer, uint32_t length) {
    uint16_t crc;

    _select();

    // read until start byte (0xFE)
    if (false == _wait_token(SPI_START_BLOCK)) {
        debug_if(SD_DBG, "Read timeout\n");
        _deselect();
        return SD_BLOCK_DEVICE_ERROR_NO_RESPONSE;
    }

    // read data
    _spi.write(NULL, 0, (char*)buffer, length);

    // Read the CRC16 checksum for the data block
    crc = (_spi.write(SPI_FILL_CHAR) << 8);
    crc |= _spi.write(SPI_FILL_CHAR);

    _deselect();
    return 0;
}

uint8_t SDBlockDevice::_write(const uint8_t *buffer, uint8_t token, uint32_t length) {
    uint16_t crc = 0xFFFF;
    uint8_t response = 0xFF;

    // Select card
    _select();

    /* If previous write is in progress, the card will drive DO low again when reselected.
     * Therefore a preceding busy check, check if card is busy prior to each command and
     * data packet, instead of post wait, can eliminate busy wait time */
    if( false == _wait_ready(SD_COMMAND_TIMEOUT)) {
        debug_if(SD_DBG, "Card not ready yet \n");
    }

    // indicate start of block
    _spi.write(token);

    // write the data
    _spi.write((char*)buffer, length, NULL, 0);

    // write the checksum CRC16
    _spi.write(crc >> 8);
    _spi.write(crc);

    // check the response token
    response = _spi.write(SPI_FILL_CHAR);
    _deselect();
    return (response & SPI_DATA_RESPONSE_MASK);
}

static uint32_t ext_bits(unsigned char *data, int msb, int lsb) {
    uint32_t bits = 0;
    uint32_t size = 1 + msb - lsb;
    for (uint32_t i = 0; i < size; i++) {
        uint32_t position = lsb + i;
        uint32_t byte = 15 - (position >> 3);
        uint32_t bit = position & 0x7;
        uint32_t value = (data[byte] >> bit) & 1;
        bits |= value << i;
    }
    return bits;
}

uint32_t SDBlockDevice::_sd_sectors() {
    uint32_t c_size, c_size_mult, read_bl_len;
    uint32_t block_len, mult, blocknr;
    uint32_t hc_c_size;
    bd_size_t blocks = 0, capacity = 0;

    // CMD9, Response R2 (R1 byte + 16-byte block read)
    if (_cmd(CMD9_SEND_CSD, 0x0) != 0x0) {
        debug_if(SD_DBG, "Didn't get a response from the disk\n");
        return 0;
    }
    uint8_t csd[16];
    if (_read_bytes(csd, 16) != 0) {
        debug_if(SD_DBG, "Couldn't read csd response from disk\n");
        return 0;
    }

    // csd_structure : csd[127:126]
    int csd_structure = ext_bits(csd, 127, 126);
    switch (csd_structure) {
        case 0:
            c_size = ext_bits(csd, 73, 62);              // c_size        : csd[73:62]
            c_size_mult = ext_bits(csd, 49, 47);         // c_size_mult   : csd[49:47]
            read_bl_len = ext_bits(csd, 83, 80);         // read_bl_len   : csd[83:80] - the *maximum* read block length
            block_len = 1 << read_bl_len;                // BLOCK_LEN = 2^READ_BL_LEN
            mult = 1 << (c_size_mult + 2);               // MULT = 2^C_SIZE_MULT+2 (C_SIZE_MULT < 8)
            blocknr = (c_size + 1) * mult;               // BLOCKNR = (C_SIZE+1) * MULT
            capacity = blocknr * block_len;              // memory capacity = BLOCKNR * BLOCK_LEN
            blocks = capacity / _block_size;
            debug_if(SD_DBG, "Standard Capacity: c_size: %d \n", c_size);
            debug_if(SD_DBG, "Sectors: 0x%x : %llu\n", blocks, blocks);
            debug_if(SD_DBG, "Capacity: 0x%x : %llu MB\n", capacity, (capacity/(1024U*1024U)));

            // ERASE_BLK_EN = 1: Erase in multiple of 512 bytes supported
            if (ext_bits(csd, 46, 46)) {
                _erase_size = BLOCK_SIZE_HC;
            } else {
                // ERASE_BLK_EN = 1: Erase in multiple of SECTOR_SIZE supported
                _erase_size = ext_bits(csd, 45, 39);
                if (_erase_size < _block_size) {
                    _erase_size = _block_size;
                }
            }
            break;

        case 1:
            hc_c_size = ext_bits(csd, 69, 48);            // device size : C_SIZE : [69:48]
            blocks = (hc_c_size+1) << 10;                 // block count = C_SIZE+1) * 1K byte (512B is block size)
            debug_if(SD_DBG, "SDHC/SDXC Card: hc_c_size: %d \n", hc_c_size);
            debug_if(SD_DBG, "Sectors: 0x%x : %llu\n", blocks, blocks);
            debug_if(SD_DBG, "Capacity: %llu MB\n", (blocks/(2048U)));
            // ERASE_BLK_EN is fixed to 1, which means host can erase one or multiple of 512 bytes.
            _erase_size = BLOCK_SIZE_HC;
            break;

        default:
            debug_if(SD_DBG, "CSD struct unsupported\r\n");
            return 0;
    };
    return blocks;
}

// SPI function to wait till chip is ready and sends start token
bool SDBlockDevice::_wait_token(uint8_t token) {
    _spi_timer.reset();
    _spi_timer.start();

    do {
        if (token == _spi.write(SPI_FILL_CHAR)) {
            _spi_timer.stop();
            return true;
        }
    } while (_spi_timer.read_ms() < 300);       // Wait for 300 msec for start token
    _spi_timer.stop();
    debug_if(SD_DBG, "_wait_token: timeout\n");
    return false;
}

// SPI function to wait till chip is ready
// The host controller should wait for end of the process until DO goes high (a 0xFF is received).
bool SDBlockDevice::_wait_ready(uint16_t ms) {
    uint8_t response;
    _spi_timer.reset();
    _spi_timer.start();
    do {
        response = _spi.write(SPI_FILL_CHAR);
        if (response == 0xFF) {
            _spi_timer.stop();
            return true;
        }
    } while (_spi_timer.read_ms() < ms);
    _spi_timer.stop();
    return false;
}

// SPI function to wait for count
void SDBlockDevice::_spi_wait(uint8_t count)
{
    for (uint8_t i = 0; i < count; ++i) {
        _spi.write(SPI_FILL_CHAR);
    }
}

void SDBlockDevice::_spi_init() {
    _spi.lock();
    // Set to SCK for initialization, and clock card with cs = 1
    _spi.frequency(_init_sck);
    _spi.format(8, 0);
    _spi.set_default_write_value(SPI_FILL_CHAR);
    // Initial 74 cycles required for few cards, before selecting SPI mode
    _cs = 1;
    _spi_wait(10);
    _spi.unlock();
}

void SDBlockDevice::_select() {
    _spi.lock();
    _cs = 0;
}

void SDBlockDevice::_deselect() {
    _cs = 1;
    _spi.unlock();
}
#endif  /* DEVICE_SPI */
