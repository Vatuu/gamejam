/***************************************************************
                            usb.c
                               
Allows USB communication between an N64 flashcart and the PC
using UNFLoader.
https://github.com/buu342/N64-UNFLoader
***************************************************************/

#include <ultra64.h>
#include <string.h>
#include "usb.h"


/*********************************
       Standard Definitions
*********************************/

// Input/Output buffer size. Keep at 512
#define BUFFER_SIZE       512

// Cart definitions
#define CART_NONE      0
#define CART_64DRIVE   2
#define CART_EVERDRIVE 3

// USB Memory location
#define DEBUG_ADDRESS  0x03F00000 // Put the debug area at the 63MB area in ROM space
#define DEBUG_ADDRESS_SIZE 1*1024*1024


/*********************************
     Parallel Interface macros
*********************************/

#define N64_PI_ADDRESS    0xA4600000

#define N64_PI_RAMADDRESS  0x00
#define N64_PI_PIADDRESS   0x04
#define N64_PI_READLENGTH  0x08
#define N64_PI_WRITELENGTH 0x0C
#define N64_PI_STATUS      0x10


/*********************************
          64Drive macros
*********************************/

// Cartridge Interface definitions. Obtained from 64Drive's Spec Sheet
#define D64_BASE_ADDRESS   0xB0000000
#define D64_CIREG_ADDRESS  0x08000000
#define D64_CIBASE_ADDRESS 0xB8000000

#define D64_REGISTER_STATUS  0x00000200
#define D64_REGISTER_COMMAND 0x00000208
#define D64_REGISTER_LBA     0x00000210
#define D64_REGISTER_LENGTH  0x00000218
#define D64_REGISTER_RESULT  0x00000220

#define D64_REGISTER_MAGIC    0x000002EC
#define D64_REGISTER_VARIANT  0x000002F0
#define D64_REGISTER_BUTTON   0x000002F8
#define D64_REGISTER_REVISION 0x000002FC

#define D64_REGISTER_USBCOMSTAT 0x00000400
#define D64_REGISTER_USBP0R0    0x00000404
#define D64_REGISTER_USBP1R1    0x00000408

#define D64_ENABLE_ROMWR  0xF0
#define D64_DISABLE_ROMWR 0xF1
#define D64_COMMAND_WRITE 0x08

// Cartridge Interface return values
#define D64_MAGIC    0x55444556

#define D64_USB_IDLE        0x00
#define D64_USB_DATA        0x02
#define D64_USB_BUSY        0x0F
#define D64_USB_DISARM      0x0F
#define D64_USB_ARM         0x0A
#define D64_USB_ARMED       0x01
#define D64_USB_ARMING      0x0F
#define D64_USB_IDLEUNARMED 0x00

#define D64_CI_IDLE  0x00
#define D64_CI_BUSY  0x10
#define D64_CI_WRITE 0x20


/*********************************
       EverDrive macros
*********************************/

#define ED_BASE           0x10000000
#define ED_BASE_ADDRESS   0x1F800000
#define ED_GET_REGADD(reg)   (0xA0000000 | ED_BASE_ADDRESS | (reg))

#define ED_REG_USBCFG  0x0004
#define ED_REG_VERSION 0x0014
#define ED_REG_USBDAT  0x0400
#define ED_REG_SYSCFG  0x8000
#define ED_REG_KEY     0x8004

#define ED_USBMODE_RDNOP 0xC400
#define ED_USBMODE_RD    0xC600
#define ED_USBMODE_WRNOP 0xC000
#define ED_USBMODE_WR    0xC200

#define ED_USBSTAT_ACT   0x0200
#define ED_USBSTAT_RXF   0x0400
#define ED_USBSTAT_TXE   0x0800
#define ED_USBSTAT_POWER 0x1000
#define ED_USBSTAT_BUSY  0x2000

#define ED_REGKEY  0xAA55

#define ED3_VERSION 0xED640008
#define ED7_VERSION 0xED640013


/*********************************
        Function Prototypes
*********************************/

static void usb_findcart();
static void usb_64drive_write(int datatype, const void* data, int size);
static int  usb_64drive_poll();
static void usb_64drive_read(void* buffer, int size);
static void usb_everdrive_readreg(u32 reg, u32* result);
static void usb_everdrive_write(int datatype, const void* data, int size);
static int  usb_everdrive_poll();
static void usb_everdrive_read(void* buffer, int size);
static void usb_everdrive_writereg(u64 reg, u32 value);


/*********************************
             Globals
*********************************/

// Function pointers
void (*funcPointer_write)(int datatype, const void* data, int size);
int  (*funcPointer_poll)();
void (*funcPointer_read)(void* buffer, int size);

// USB globals
static s8 usb_cart = CART_NONE;
static u8 usb_bufferout[BUFFER_SIZE] __attribute__((aligned(16)));
static int usb_datatype = 0;
static int usb_dataleft = 0;

// Message globals
#if !USE_OSRAW
    OSMesg      dmaMessageBuf;
    OSIoMesg    dmaIOMessageBuf;
    OSMesgQueue dmaMessageQ;
#endif

// osPiRaw
#if USE_OSRAW
    extern s32 __osPiRawWriteIo(u32, u32);
    extern s32 __osPiRawReadIo(u32, u32 *);
    extern s32 __osPiRawStartDma(s32, u32, void *, u32);
    
    #define osPiRawWriteIo(a, b) __osPiRawWriteIo(a, b)
    #define osPiRawReadIo(a, b) __osPiRawReadIo(a, b)
    #define osPiRawStartDma(a, b, c, d) __osPiRawStartDma(a, b, c, d)
#endif


/*********************************
           USB functions
*********************************/

/*==============================
    usb_initialize
    Initializes the USB buffers and pointers
    @returns 1 if the USB initialization was successful, 0 if not
==============================*/

char usb_initialize()
{
    // Initialize the debug related globals
    memset(usb_bufferout, 0, BUFFER_SIZE);
        
    // Create the message queue
    #if !USE_OSRAW
        osCreateMesgQueue(&dmaMessageQ, &dmaMessageBuf, 1);
    #endif
    
    // Find the flashcart
    usb_findcart();

    // Set the function pointers based on the flashcart
    switch(usb_cart)
    {
        case CART_64DRIVE:
            funcPointer_write = usb_64drive_write;
            funcPointer_poll  = usb_64drive_poll;
            funcPointer_read  = usb_64drive_read;
            break;
        case CART_EVERDRIVE:
            funcPointer_write = usb_everdrive_write;
            funcPointer_poll  = usb_everdrive_poll;
            funcPointer_read  = usb_everdrive_read;
            break;
        default:
            return 0;
    }
    return 1;
}


/*==============================
    usb_findcart
    Checks if the game is running on a 64Drive or an EverDrive.
==============================*/

static void usb_findcart()
{
    u32 buff;
    
    // Read the cartridge and check if we have a 64Drive.
    #if USE_OSRAW
        osPiRawReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_MAGIC, &buff);
    #else
        osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_MAGIC, &buff);
    #endif
    if (buff == D64_MAGIC)
    {
        usb_cart = CART_64DRIVE;
        return;
    }
    
    // Since we didn't find a 64Drive, let's assume we have an EverDrive
    // Write the key to unlock the registers, then read the version register
    usb_everdrive_writereg(ED_REG_KEY, ED_REGKEY);
    usb_everdrive_readreg(ED_REG_VERSION, &buff);
    
    // Check if we have an EverDrive
    if (buff == ED7_VERSION || buff == ED3_VERSION)
    {        
        // Initialize the PI
        IO_WRITE(PI_STATUS_REG, 3);
        IO_WRITE(PI_BSD_DOM1_LAT_REG, 0x40);
        IO_WRITE(PI_BSD_DOM1_PWD_REG, 0x12);
        IO_WRITE(PI_BSD_DOM1_PGS_REG, 0x07);
        IO_WRITE(PI_BSD_DOM1_RLS_REG, 0x03);
        IO_WRITE(PI_BSD_DOM2_LAT_REG, 0x05);
        IO_WRITE(PI_BSD_DOM2_PWD_REG, 0x0C);
        IO_WRITE(PI_BSD_DOM2_PGS_REG, 0x0D);
        IO_WRITE(PI_BSD_DOM2_RLS_REG, 0x02);
        IO_WRITE(PI_BSD_DOM1_LAT_REG, 0x04);
        IO_WRITE(PI_BSD_DOM1_PWD_REG, 0x0C);
        
        // Set the USB mode
        usb_everdrive_writereg(ED_REG_SYSCFG, 0);
        usb_everdrive_writereg(ED_REG_USBCFG, ED_USBMODE_RDNOP);
        
        // Set the cart to EverDrive
        usb_cart = CART_EVERDRIVE;
        return;
    }
}


/*==============================
    usb_write
    Writes data to the USB.
    @param The DATATYPE that is being sent
    @param A buffer with the data to send
    @param The size of the data being sent
==============================*/

void usb_write(int datatype, const void* data, int size)
{
    // If no debug cart exists, stop
    if (usb_cart == CART_NONE)
        return;
        
    // Call the correct write function
    funcPointer_write(datatype, data, size);
}


/*==============================
    usb_poll
    Returns the header of data being received via USB
    The first byte contains the data type, the next 3 the size left to read
    @return The data header, or 0
==============================*/

int usb_poll()
{
    // If no debug cart exists, stop
    if (usb_cart == CART_NONE)
        return 0;
        
    // Call the correct read function
    return funcPointer_poll();
}


/*==============================
    usb_read
    Reads bytes from the USB into the provided buffer
    @param The buffer to put the read data in
    @param The number of bytes to read
==============================*/

void usb_read(void* buffer, int size)
{       
    // If no debug cart exists, stop
    if (usb_cart == CART_NONE)
        return;
        
    // Call the correct read function
    funcPointer_read(buffer, size);
}


/*********************************
        64Drive functions
*********************************/

/*==============================
    usb_64drive_wait
    Wait until the 64Drive is ready
    @return 0 if success or -1 if failure
==============================*/

static s8 usb_64drive_wait()
{
    u32 ret;
    u32 timeout = 0; // I wanted to use osGetTime() but that requires the VI manager
    
    // Wait until the cartridge interface is ready
    do
    {
        #if USE_OSRAW
            osPiRawReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_STATUS, &ret);
        #else
            osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_STATUS, &ret);
        #endif
        
        // Took too long, abort
        if((timeout++) > 1000000)
            return -1;
    }
    while((ret >> 8) & D64_CI_BUSY);
    
    // Success
    return 0;
}


/*==============================
    usb_64drive_setwritable
    Set the write mode on the 64Drive
    @param A boolean with whether to enable or disable
==============================*/

static void usb_64drive_setwritable(u8 enable)
{
    usb_64drive_wait();
    #if USE_OSRAW
        osPiRawWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_COMMAND, enable ? D64_ENABLE_ROMWR : D64_DISABLE_ROMWR);
    #else
        osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_COMMAND, enable ? D64_ENABLE_ROMWR : D64_DISABLE_ROMWR);
    #endif
    usb_64drive_wait();
}


/*==============================
    usb_64drive_waitidle
    Waits for the 64Drive's USB to be idle
==============================*/

static void usb_64drive_waitidle()
{
    u32 status;
    do 
    {
        #if USE_OSRAW
            osPiRawReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &status);
        #else
            osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &status);
        #endif
        status = (status >> 4) & D64_USB_BUSY;
    }
    while(status != D64_USB_IDLE);
}


/*==============================
    usb_64drive_waitdata
    Waits for the 64Drive's USB be ablt to receive data
==============================*/

static void usb_64drive_waitdata()
{
    u32 status;
    do
    {
        #if USE_OSRAW
            osPiRawReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &status);
        #else
            osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &status);
        #endif
        status &= 0x0F;
    }
    while (status == D64_USB_IDLEUNARMED || status == D64_USB_ARMED);
}


/*==============================
    usb_64drive_waitdisarmed
    Waits for the 64Drive's USB to be disarmed
==============================*/

static void usb_64drive_waitdisarmed()
{
    u32 status;
    do
    {
        #if USE_OSRAW
            osPiRawReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &status);
        #else
            osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &status);
        #endif
        status &= 0x0F;
    }
    while (status != D64_USB_IDLEUNARMED);
}


/*==============================
    usb_64drive_write
    Sends data through USB from the 64Drive
    @param The DATATYPE that is being sent
    @param A buffer with the data to send
    @param The size of the data being sent
==============================*/

static void usb_64drive_write(int datatype, const void* data, int size)
{
    int left = size;
    int read = 0;
    
    // Spin until the write buffer is free and then set the cartridge to write mode
    usb_64drive_waitidle();
    usb_64drive_setwritable(TRUE);
    
    // Write data to SDRAM until we've finished
    while (left > 0)
    {
        int block = left;
        if (block > BUFFER_SIZE)
            block = BUFFER_SIZE;
            
        // Copy the data to the global buffer
        memcpy(usb_bufferout, (void*)((char*)data+read), block);

        // If the data was not 32-bit aligned, pad the buffer
        if(block < BUFFER_SIZE && size%4 != 0)
        {
            u32 i;
            u32 size_new = (size & ~3)+4;
            block += size_new-size;
            for(i=size; i<size_new; i++) 
                usb_bufferout[i] = 0;
            size = size_new;
        }
        
        // Spin until the write buffer is free
        usb_64drive_waitidle();
        
        // Set up DMA transfer between RDRAM and the PI
        osWritebackDCache(usb_bufferout, block);
        #if USE_OSRAW
            osPiRawStartDma(OS_WRITE, 
                         D64_BASE_ADDRESS + DEBUG_ADDRESS + read, 
                         usb_bufferout, block);
        #else
            osPiStartDma(&dmaIOMessageBuf, OS_MESG_PRI_NORMAL, OS_WRITE, 
                         D64_BASE_ADDRESS + DEBUG_ADDRESS + read, 
                         usb_bufferout, block, &dmaMessageQ);
            (void)osRecvMesg(&dmaMessageQ, NULL, OS_MESG_BLOCK);
        #endif
        
        // Keep track of what we've read so far
        left -= block;
        read += block;
    }
    
    // Send the data through USB
    #if USE_OSRAW
        osPiRawWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP0R0, DEBUG_ADDRESS >> 1);
        osPiRawWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP1R1, (size & 0xFFFFFF) | (datatype << 24));
        osPiRawWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, D64_COMMAND_WRITE);
    #else
        osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP0R0, DEBUG_ADDRESS >> 1);
        osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP1R1, (size & 0xFFFFFF) | (datatype << 24));
        osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, D64_COMMAND_WRITE);
    #endif
        
    // Spin until the write buffer is free and then disable write mode
    usb_64drive_waitidle();
    usb_64drive_setwritable(FALSE);
}


/*==============================
    usb_64drive_poll
    Returns the header of data being received via USB on the 64Drive
    The first byte contains the data type, the next 3 the size left to read
    @return The data header, or 0
==============================*/

static int usb_64drive_poll()
{   
    u32 ret;
    
    // If there's still data that needs to be read, return the header with the data left
    if (usb_dataleft != 0)
        return USBHEADER_CREATE(usb_datatype, usb_dataleft);
    
    // Check if we've received data
    #if USE_OSRAW
        osPiRawReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &ret);
    #else
        osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &ret);
    #endif
    
    // No data, stop here
    if (ret != D64_USB_DATA)
        return 0;
        
    // Check if the USB is armed, and arm it if it isn't
    #if USE_OSRAW
        osPiRawReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &ret); 
    #else
        osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &ret); 
    #endif
    if (ret != D64_USB_ARMING || ret != D64_USB_ARMED)
    {
        // Ensure the 64Drive is idle
        usb_64drive_waitidle();
        
        // Arm the USB FIFO DMA
        #if USE_OSRAW
            osPiRawWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP0R0, DEBUG_ADDRESS >> 1);
            osPiRawWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP1R1, BUFFER_SIZE & 0xFFFFFF);
            osPiRawWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, D64_USB_ARM);
        #else
            osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP0R0, DEBUG_ADDRESS >> 1);
            osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP1R1, BUFFER_SIZE & 0xFFFFFF);
            osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, D64_USB_ARM);
        #endif
    }  
        
    // Wait for the data to arrive
    usb_64drive_waitdata();
        
    // Read the data header
    #if USE_OSRAW
        osPiRawReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP0R0, &ret); 
    #else
        osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP0R0, &ret); 
    #endif
    
    // Disarm the USB
    #if USE_OSRAW
        osPiRawWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, D64_USB_DISARM); 
    #else
        osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, D64_USB_DISARM); 
    #endif
    usb_64drive_waitdisarmed();
    
    // Store information about the incoming data, then return the header
    usb_datatype = USBHEADER_GETTYPE(ret);
    usb_dataleft = USBHEADER_GETSIZE(ret);
    return ret;
}


/*==============================
    usb_64drive_read
    Reads bytes from the 64Drive USB into the provided buffer
    @param The buffer to put the read data in
    @param The number of bytes to read
==============================*/

static void usb_64drive_read(void* buffer, int nbytes)
{
    u32 ret;
    
    // Check if we've received data
    #if USE_OSRAW
        osPiRawReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &ret);
    #else
        osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &ret);
    #endif
    if (ret != D64_USB_DATA)
        return;
        
    // Check if the USB is armed, and arm it if it isn't
    #if USE_OSRAW
        osPiRawReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &ret); 
    #else
        osPiReadIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, &ret); 
    #endif
    if (ret != D64_USB_ARMING || ret != D64_USB_ARMED)
    {
        // Ensure the 64Drive is idle
        usb_64drive_waitidle();
        
        // Arm the USB FIFO DMA
        #if USE_OSRAW
            osPiRawWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP0R0, DEBUG_ADDRESS >> 1);
            osPiRawWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP1R1, BUFFER_SIZE & 0xFFFFFF);
            osPiRawWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, D64_USB_ARM);
        #else
            osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP0R0, DEBUG_ADDRESS >> 1);
            osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBP1R1, BUFFER_SIZE & 0xFFFFFF);
            osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, D64_USB_ARM);
        #endif
    }  
    
    // Wait for data
    usb_64drive_waitdata();

    // Set up DMA transfer between RDRAM and the PI
    osWritebackDCache(buffer, nbytes);
    // osWritebackDCacheAll();
    #if USE_OSRAW
        osPiRawStartDma(OS_READ, 
                     D64_BASE_ADDRESS + DEBUG_ADDRESS, buffer, 
                     nbytes);
    #else
        osPiStartDma(&dmaIOMessageBuf, OS_MESG_PRI_NORMAL, OS_READ, 
                     D64_BASE_ADDRESS + DEBUG_ADDRESS, buffer, 
                     nbytes, &dmaMessageQ);
        (void)osRecvMesg(&dmaMessageQ, NULL, OS_MESG_BLOCK);
    #endif
    usb_dataleft -= nbytes;
    
    // If we're out of USB data, we don't need the header anymore...
    if (usb_dataleft == 0)
        usb_datatype = 0;
    
    // Disarm the USB
    #if USE_OSRAW
        osPiRawWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, D64_USB_DISARM); 
    #else
        osPiWriteIo(D64_CIBASE_ADDRESS + D64_REGISTER_USBCOMSTAT, D64_USB_DISARM); 
    #endif
    usb_64drive_waitdisarmed();
}


/*********************************
       EverDrive functions
*********************************/

/*==============================
    usb_everdrive_wait_pidma
    Spins until the EverDrive's DMA is ready
==============================*/

static void usb_everdrive_wait_pidma() 
{
    u32 status;
    do
    {
        status = *(volatile unsigned long *)(N64_PI_ADDRESS + N64_PI_STATUS);
        status &= (PI_STATUS_DMA_BUSY | PI_STATUS_IO_BUSY);
    }
    while (status);
}


/*==============================
    usb_everdrive_readdata
    Reads data from a specific address on the EverDrive
    @param The buffer with the data
    @param The register address to write to the PI
    @param The size of the data
==============================*/

static void usb_everdrive_readdata(void* buff, u32 pi_address, u32 len) 
{
    // Correct the PI address
    pi_address &= 0x1FFFFFFF;

    // Set up DMA transfer between RDRAM and the PI
    osInvalDCache(buff, len);
    #if USE_OSRAW
        osPiRawStartDma(OS_READ, 
                     pi_address, buff, 
                     len);
    #else
        osPiStartDma(&dmaIOMessageBuf, OS_MESG_PRI_NORMAL, OS_READ, 
                     pi_address, buff, 
                     len, &dmaMessageQ);
        (void)osRecvMesg(&dmaMessageQ, NULL, OS_MESG_BLOCK);
    #endif

    // Write the data to the PI
    usb_everdrive_wait_pidma();
    IO_WRITE(PI_STATUS_REG, 3);
    *(volatile unsigned long *)(N64_PI_ADDRESS + N64_PI_RAMADDRESS) = (u32)buff;
    *(volatile unsigned long *)(N64_PI_ADDRESS + N64_PI_PIADDRESS) = pi_address;
    *(volatile unsigned long *)(N64_PI_ADDRESS + N64_PI_READLENGTH) = len-1;
    usb_everdrive_wait_pidma();
}


/*==============================
    usb_everdrive_readreg
    Reads data from a specific register on the EverDrive
    @param The register to read from
    @param A pointer to write the read value to
==============================*/

static void usb_everdrive_readreg(u32 reg, u32* result) 
{
    usb_everdrive_readdata(result, ED_GET_REGADD(reg), sizeof(u32));
}


/*==============================
    usb_everdrive_writedata
    Writes data to a specific address on the EverDrive
    @param A buffer with the data to write
    @param The register address to write to the PI
    @param The length of the data
==============================*/

static void usb_everdrive_writedata(void* buff, u32 pi_address, u32 len) 
{
    // Correct the PI address
    pi_address &= 0x1FFFFFFF;
    
    // Set up DMA transfer between RDRAM and the PI
    osWritebackDCache(buff, len);
    #if USE_OSRAW
        osPiRawStartDma(OS_WRITE, 
                     pi_address, buff, 
                     len);
    #else
        osPiStartDma(&dmaIOMessageBuf, OS_MESG_PRI_NORMAL, OS_WRITE, 
                     pi_address, buff, 
                     len, &dmaMessageQ);
        (void)osRecvMesg(&dmaMessageQ, NULL, OS_MESG_BLOCK);
    #endif
    
    // Write the data to the PI
    usb_everdrive_wait_pidma();
    IO_WRITE(PI_STATUS_REG, 3);
    *(volatile unsigned long *)(N64_PI_ADDRESS + N64_PI_RAMADDRESS) = (u32)buff;
    *(volatile unsigned long *)(N64_PI_ADDRESS + N64_PI_PIADDRESS) = pi_address;
    *(volatile unsigned long *)(N64_PI_ADDRESS + N64_PI_WRITELENGTH) = len-1;
    usb_everdrive_wait_pidma();
}


/*==============================
    usb_everdrive_writereg
    Writes data to a specific register on the EverDrive
    @param The register to write to
    @param The value to write to the register
==============================*/

static void usb_everdrive_writereg(u64 reg, u32 value) 
{
    usb_everdrive_writedata(&value, ED_GET_REGADD(reg), sizeof(u32));
}


/*==============================
    usb_everdrive_usbbusy
    Spins until the USB is no longer busy
==============================*/

static void usb_everdrive_usbbusy() 
{
    u32 val;
    do 
    {
        usb_everdrive_readreg(ED_REG_USBCFG, &val);
    } 
    while ((val & ED_USBSTAT_ACT) != 0);
}


/*==============================
    usb_everdrive_canread
    Checks if the EverDrive's USB can read
    @return 1 if it can read, 0 if not
==============================*/

static u8 usb_everdrive_canread() 
{
    u32 val, status = ED_USBSTAT_POWER;
    
    // Read the USB register and check its status
    usb_everdrive_readreg(ED_REG_USBCFG, &val);
    status = val & (ED_USBSTAT_POWER | ED_USBSTAT_RXF);
    return status == ED_USBSTAT_POWER;
}


/*==============================
    usb_everdrive_write
    Sends data through USB from the EverDrive
    @param The DATATYPE that is being sent
    @param A buffer with the data to send
    @param The size of the data being sent
==============================*/

static void usb_everdrive_write(int datatype, const void* data, int size)
{
    char wrotecmp = 0;
    char cmp[] = {'C', 'M', 'P', 'H'};
    int read = 0;
    int left = size;
    int offset = 8;
    u32 header = (size & 0x00FFFFFF) | (datatype << 24);
    
    // Put in the DMA header along with length and type information in the global buffer
    usb_bufferout[0] = 'D';
    usb_bufferout[1] = 'M';
    usb_bufferout[2] = 'A';
    usb_bufferout[3] = '@';
    usb_bufferout[4] = (header >> 24) & 0xFF;
    usb_bufferout[5] = (header >> 16) & 0xFF;
    usb_bufferout[6] = (header >> 8)  & 0xFF;
    usb_bufferout[7] = header & 0xFF;
    
    // Write data to USB until we've finished
    while (left > 0)
    {
        int block = left;
        int blocksend, baddr;
        if (block+offset > BUFFER_SIZE)
            block = BUFFER_SIZE-offset;
            
        // Copy the data to the next available spots in the global buffer
        memcpy(usb_bufferout+offset, (void*)((char*)data+read), block);
        
        // Restart the loop to write the CMP signal if we've finished
        if (!wrotecmp && read+block >= size)
        {
            left = 4;
            offset = block+offset;
            data = cmp;
            wrotecmp = 1;
            read = 0;
            continue;
        }
        
        // Ensure the data is 16 byte aligned and the block address is correct
        blocksend = (block+offset)+15 - ((block+offset)+15)%16;
        baddr = BUFFER_SIZE - blocksend;

        // Set USB to write mode and send data through USB
        usb_everdrive_writereg(ED_REG_USBCFG, ED_USBMODE_WRNOP);
        usb_everdrive_writedata(usb_bufferout, ED_GET_REGADD(ED_REG_USBDAT + baddr), blocksend);
        
        // Set USB to write mode with the new address and wait for USB to end
        usb_everdrive_writereg(ED_REG_USBCFG, ED_USBMODE_WR | baddr);
        usb_everdrive_usbbusy();
        
        // Keep track of what we've read so far
        left -= block;
        read += block;
        offset = 0;
    }
}


/*==============================
    usb_everdrive_poll
    Returns the header of data being received via USB on the EverDrive
    The first byte contains the data type, the next 3 the size left to read
    @return The data header, or 0
==============================*/

static int usb_everdrive_poll()
{
    char buff[8];
    
    // If there's still data that needs to be read, return how much
    if (usb_dataleft != 0)
        return USBHEADER_CREATE(usb_datatype, usb_dataleft);
    
    // Wait for the USB to be ready
    usb_everdrive_usbbusy();
    
    // Check if the USB is ready to be read
    if (!usb_everdrive_canread())
        return 0;
    
    // Read the first 8 bytes that are being received and check if they're valid
    usb_everdrive_read(&buff, 8);
    if (buff[0] != 'D' || buff[1] != 'M' || buff[2] != 'A' || buff[3] != '@')
        return 0;
        
    // Store information about the incoming data, then return the header
    usb_datatype = (int)buff[4];
    usb_dataleft = (int)buff[5]<<16 | (int)buff[6]<<8 | (int)buff[7]<<0;
    return USBHEADER_CREATE(usb_datatype, usb_dataleft);
}


/*==============================
    usb_everdrive_read
    Reads bytes from the EverDrive USB into the provided buffer
    @param The buffer to put the read data in
    @param The number of bytes to read
==============================*/

static void usb_everdrive_read(void* buffer, int nbytes)
{
    char buff[4];
    char checkCMP = 0;
    u16 blen, baddr;

    // If there's a CMP signal at the end of this data block, mark to read it
    if (usb_dataleft > 0 && usb_dataleft-nbytes == 0)
        checkCMP = 1;
    
    while (nbytes) 
    {
        blen = 512; //rx block len
        if (blen > nbytes)
            blen = nbytes;
        baddr = 512 - blen; //address in fpga internal buffer. requested data length equal to 512-int buffer addr
        usb_everdrive_writereg(ED_REG_USBCFG, ED_USBMODE_RD | baddr); //usb read request. fpga will receive usb bytes until the buffer address reaches 512

        usb_everdrive_usbbusy(); //wait until requested data amount will be transferred to the internal buffer

        usb_everdrive_readdata(buffer, ED_GET_REGADD(ED_REG_USBDAT + baddr), blen); //get data from internal buffer

        buffer += blen;
        nbytes -= blen;
        usb_dataleft -= blen;
    }
    
    // If we're out of USB data, we don't need the header anymore...
    if (usb_dataleft == 0)
        usb_datatype = 0;
    
    // Read the CMP signal to finish, if needed
    if (checkCMP)
        usb_everdrive_read(&buff, 4);
}