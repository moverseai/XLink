// project
#define MVLOG_UNIT_NAME xLinkUsb

// libraries
#ifdef XLINK_LIBUSB_LOCAL
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#include "XLink/XLinkLog.h"
#include "XLink/XLinkPlatform.h"
#include "XLink/XLinkPublicDefines.h"
#include "usb_mx_id.h"
#include "usb_host.h"

// std
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>

constexpr static int MAXIMUM_PORT_NUMBERS = 7;
using VidPid = std::pair<uint16_t, uint16_t>;
static const int MX_ID_TIMEOUT_MS = 100;

static constexpr auto DEFAULT_OPEN_TIMEOUT = std::chrono::seconds(5);
static constexpr auto DEFAULT_WRITE_TIMEOUT = 2000;
static constexpr std::chrono::milliseconds DEFAULT_CONNECT_TIMEOUT{20000};
static constexpr std::chrono::milliseconds DEFAULT_SEND_FILE_TIMEOUT{10000};
static constexpr auto USB1_CHUNKSZ = 64;

static constexpr int USB_ENDPOINT_IN = 0x81;
static constexpr int USB_ENDPOINT_OUT = 0x01;

static constexpr int XLINK_USB_DATA_TIMEOUT = 0;

static unsigned int bulk_chunklen = DEFAULT_CHUNKSZ;
static int write_timeout = DEFAULT_WRITE_TIMEOUT;
static int initialized;

struct UsbSetupPacket {
  uint8_t  requestType;
  uint8_t  request;
  uint16_t value;
  uint16_t index;
  uint16_t length;
};

static UsbSetupPacket bootBootloaderPacket{
    0x00, // bmRequestType: device-directed
    0xF5, // bRequest: custom
    0x0DA1, // wValue: custom
    0x0000, // wIndex
    0 // not used
};



static std::mutex mutex;
static libusb_context* context;

int usbInitialize(void* options){
    #ifdef __ANDROID__
        // If Android, set the options as JavaVM (to default context)
        if(options != nullptr){
            libusb_set_option(NULL, libusb_option::LIBUSB_OPTION_ANDROID_JAVAVM, options);
        }
    #endif

    return libusb_init(&context);
}

struct pair_hash {
    template <class T1, class T2>
    std::size_t operator() (const std::pair<T1, T2> &pair) const {
        return std::hash<T1>()(pair.first) ^ std::hash<T2>()(pair.second);
    }
};

static std::unordered_map<VidPid, XLinkDeviceState_t, pair_hash> vidPidToDeviceState = {
    {{0x03E7, 0x2485}, X_LINK_UNBOOTED},
    {{0x03E7, 0xf63b}, X_LINK_BOOTED},
    {{0x03E7, 0xf63c}, X_LINK_BOOTLOADER},
};

static std::string getLibusbDevicePath(libusb_device *dev);
static libusb_error getLibusbDeviceMxId(XLinkDeviceState_t state, std::string devicePath, const libusb_device_descriptor* pDesc, libusb_device *dev, std::string& outMxId);
static const char* xlink_libusb_strerror(int x);



extern "C" xLinkPlatformErrorCode_t getUSBDevices(const deviceDesc_t in_deviceRequirements,
                                                     deviceDesc_t* out_foundDevices, int sizeFoundDevices,
                                                     unsigned int *out_amountOfFoundDevices) {

    std::lock_guard<std::mutex> l(mutex);

    // Get list of usb devices
    static libusb_device **devs = NULL;
    auto numDevices = libusb_get_device_list(context, &devs);
    if(numDevices < 0) {
        mvLog(MVLOG_DEBUG, "Unable to get USB device list: %s", xlink_libusb_strerror(numDevices));
        return X_LINK_PLATFORM_ERROR;
    }

    // Initialize mx id cache
    usb_mx_id_cache_init();

    // Loop over all usb devices, increase count only if myriad device
    int numDevicesFound = 0;
    for(ssize_t i = 0; i < numDevices; i++) {
        if(devs[i] == nullptr) continue;

        if(numDevicesFound >= sizeFoundDevices){
            break;
        }

        // Get device descriptor
        struct libusb_device_descriptor desc;
        auto res = libusb_get_device_descriptor(devs[i], &desc);
        if (res < 0) {
            mvLog(MVLOG_DEBUG, "Unable to get USB device descriptor: %s", xlink_libusb_strerror(res));
            continue;
        }

        VidPid vidpid{desc.idVendor, desc.idProduct};

        if(vidPidToDeviceState.count(vidpid) > 0){
            // Device found

            // Device status
            XLinkError_t status = X_LINK_SUCCESS;

            // Get device state
            XLinkDeviceState_t state = vidPidToDeviceState.at(vidpid);
            // Check if compare with state
            if(in_deviceRequirements.state != X_LINK_ANY_STATE && state != in_deviceRequirements.state){
                // Current device doesn't match the "filter"
                continue;
            }

            // Get device name
            std::string devicePath = getLibusbDevicePath(devs[i]);
            // Check if compare with name
            std::string requiredName(in_deviceRequirements.name);
            if(requiredName.length() > 0 && requiredName != devicePath){
                // Current device doesn't match the "filter"
                continue;
            }

            // Get device mxid
            std::string mxId;
            libusb_error rc = getLibusbDeviceMxId(state, devicePath, &desc, devs[i], mxId);
            switch (rc)
            {
            case LIBUSB_SUCCESS:
                status = X_LINK_SUCCESS;
                break;
            case LIBUSB_ERROR_ACCESS:
                status = X_LINK_INSUFFICIENT_PERMISSIONS;
                break;
            default:
                status = X_LINK_ERROR;
                break;
            }

            // compare with MxId
            std::string requiredMxId(in_deviceRequirements.mxid);
            if(requiredMxId.length() > 0 && requiredMxId != mxId){
                // Current device doesn't match the "filter"
                continue;
            }

            // TODO(themarpe) - check platform

            // Everything passed, fillout details of found device
            out_foundDevices[numDevicesFound].status = status;
            out_foundDevices[numDevicesFound].platform = X_LINK_MYRIAD_X;
            out_foundDevices[numDevicesFound].protocol = X_LINK_USB_VSC;
            out_foundDevices[numDevicesFound].state = state;
            memset(out_foundDevices[numDevicesFound].name, 0, sizeof(out_foundDevices[numDevicesFound].name));
            strncpy(out_foundDevices[numDevicesFound].name, devicePath.c_str(), sizeof(out_foundDevices[numDevicesFound].name));
            memset(out_foundDevices[numDevicesFound].mxid, 0, sizeof(out_foundDevices[numDevicesFound].mxid));
            strncpy(out_foundDevices[numDevicesFound].mxid, mxId.c_str(), sizeof(out_foundDevices[numDevicesFound].mxid));
            numDevicesFound++;

        }

    }

    // Free list of usb devices
    libusb_free_device_list(devs, 1);

    // Write the number of found devices
    *out_amountOfFoundDevices = numDevicesFound;

    return X_LINK_PLATFORM_SUCCESS;
}

extern "C" xLinkPlatformErrorCode_t refLibusbDeviceByName(const char* name, libusb_device** pdev) {

    std::lock_guard<std::mutex> l(mutex);

    // Get list of usb devices
    static libusb_device **devs = NULL;
    auto numDevices = libusb_get_device_list(context, &devs);
    if(numDevices < 0) {
        mvLog(MVLOG_DEBUG, "Unable to get USB device list: %s", xlink_libusb_strerror(numDevices));
        return X_LINK_PLATFORM_ERROR;
    }

    // Loop over all usb devices, increase count only if myriad device
    bool found = false;
    for(ssize_t i = 0; i < numDevices; i++) {
        if(devs[i] == nullptr) continue;

        // Check path only
        std::string devicePath = getLibusbDevicePath(devs[i]);
        // Check if compare with name
        std::string requiredName(name);
        if(requiredName.length() > 0 && requiredName == devicePath){
            // Found, increase ref and exit the loop
            libusb_ref_device(devs[i]);
            *pdev = devs[i];
            found = true;
            break;
        }
    }

    // Free list of usb devices (unref each)
    libusb_free_device_list(devs, 1);

    if(!found){
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }

    return X_LINK_PLATFORM_SUCCESS;
}



std::string getLibusbDevicePath(libusb_device *dev) {

    std::string devicePath = "";

    // Add bus number
    uint8_t bus = libusb_get_bus_number(dev);
    devicePath += std::to_string(bus) + ".";

    // Add all subsequent port numbers
    uint8_t portNumbers[MAXIMUM_PORT_NUMBERS];
    int count = libusb_get_port_numbers(dev, portNumbers, MAXIMUM_PORT_NUMBERS);
    if (count == LIBUSB_ERROR_OVERFLOW) {
        // shouldn't happen!
        return "<error>";
    }
    if(count == 0){
        // Only bus number is available
        return devicePath;
    }

    for (int i = 0; i < count - 1; i++){
        devicePath += std::to_string(portNumbers[i]) + ".";
    }
    devicePath += std::to_string(portNumbers[count - 1]);

    // Return the device path
    return devicePath;
}

libusb_error getLibusbDeviceMxId(XLinkDeviceState_t state, std::string devicePath, const libusb_device_descriptor* pDesc, libusb_device *dev, std::string& outMxId)
{
    char mxId[XLINK_MAX_MX_ID_SIZE] = {0};

    // Default MXID - emptpy
    outMxId = "";

    // first check if entry already exists in the list (and is still valid)
    // if found, it stores it into mx_id variable
    bool found = usb_mx_id_cache_get_entry(devicePath.c_str(), mxId);

    if(found){
        mvLog(MVLOG_DEBUG, "Found cached MX ID: %s", mxId);
        outMxId = std::string(mxId);
        return LIBUSB_SUCCESS;
    } else {
        // If not found, retrieve mxId

        // get serial from usb descriptor
        libusb_device_handle *handle = NULL;
        int libusb_rc = LIBUSB_SUCCESS;

        // Open device
        libusb_rc = libusb_open(dev, &handle);
        if (libusb_rc < 0){
            // Some kind of error, either NO_MEM, ACCESS, NO_DEVICE or other
            // In all these cases, return
            // no cleanup needed
            return (libusb_error) libusb_rc;
        }


        // Retry getting MX ID for 5ms
        const std::chrono::milliseconds RETRY_TIMEOUT{5}; // 5ms
        const std::chrono::microseconds SLEEP_BETWEEN_RETRIES{100}; // 100us

        auto t1 = std::chrono::steady_clock::now();
        do {

            // if UNBOOTED state, perform mx_id retrieval procedure using small program and a read command
            if(state == X_LINK_UNBOOTED){

                // Get configuration first (From OS cache)
                int active_configuration = -1;
                if( (libusb_rc = libusb_get_configuration(handle, &active_configuration)) == 0){
                    if(active_configuration != 1){
                        mvLog(MVLOG_DEBUG, "Setting configuration from %d to 1\n", active_configuration);
                        if ((libusb_rc = libusb_set_configuration(handle, 1)) < 0) {
                            mvLog(MVLOG_ERROR, "libusb_set_configuration: %s", xlink_libusb_strerror(libusb_rc));

                            // retry
                            std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                            continue;
                        }
                    }
                } else {
                    // getting config failed...
                    mvLog(MVLOG_ERROR, "libusb_set_configuration: %s", xlink_libusb_strerror(libusb_rc));

                    // retry
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }


                // Claim interface (as we'll be doing IO on endpoints)
                if ((libusb_rc = libusb_claim_interface(handle, 0)) < 0) {
                    if(libusb_rc != LIBUSB_ERROR_BUSY){
                        mvLog(MVLOG_ERROR, "libusb_claim_interface: %s", xlink_libusb_strerror(libusb_rc));
                    }
                    // retry - most likely device busy by another app
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }


                const int send_ep = 0x01;
                int transferred = 0;

                // ///////////////////////
                // Start
                // WD Protection & MXID Retrieval Command
                transferred = 0;
                libusb_rc = libusb_bulk_transfer(handle, send_ep, ((uint8_t*) usb_mx_id_get_payload()), usb_mx_id_get_payload_size(), &transferred, MX_ID_TIMEOUT_MS);
                if (libusb_rc < 0 || usb_mx_id_get_payload_size() != transferred) {
                    mvLog(MVLOG_ERROR, "libusb_bulk_transfer (%s), transfer: %d, expected: %d", libusb_strerror(libusb_rc), transferred, usb_mx_id_get_payload_size());
                    // Mark as error and retry
                    libusb_rc = -1;
                    // retry
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }

                // MXID Read
                const int recv_ep = 0x81;
                const int expectedMxIdReadSize = 9;
                uint8_t rbuf[128];
                transferred = 0;
                libusb_rc = libusb_bulk_transfer(handle, recv_ep, rbuf, sizeof(rbuf), &transferred, MX_ID_TIMEOUT_MS);
                if (libusb_rc < 0 || expectedMxIdReadSize != transferred) {
                    mvLog(MVLOG_ERROR, "libusb_bulk_transfer (%s), transfer: %d, expected: %d", xlink_libusb_strerror(libusb_rc), transferred, expectedMxIdReadSize);
                    // Mark as error and retry
                    libusb_rc = -1;
                    // retry
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }

                // WD Protection end
                transferred = 0;
                libusb_rc = libusb_bulk_transfer(handle, send_ep, ((uint8_t*) usb_mx_id_get_payload_end()), usb_mx_id_get_payload_end_size(), &transferred, MX_ID_TIMEOUT_MS);
                if (libusb_rc < 0 || usb_mx_id_get_payload_end_size() != transferred) {
                    mvLog(MVLOG_ERROR, "libusb_bulk_transfer (%s), transfer: %d, expected: %d", xlink_libusb_strerror(libusb_rc), transferred, usb_mx_id_get_payload_end_size());
                    // Mark as error and retry
                    libusb_rc = -1;
                    // retry
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }
                // End
                ///////////////////////

                // Release claimed interface
                // ignore error as it doesn't matter
                libusb_release_interface(handle, 0);

                // Parse mxId into HEX presentation
                // There's a bug, it should be 0x0F, but setting as in MDK
                rbuf[8] &= 0xF0;

                // Convert to HEX presentation and store into mx_id
                for (int i = 0; i < expectedMxIdReadSize; i++) {
                    sprintf(mxId + 2*i, "%02X", rbuf[i]);
                }

                // Indicate no error
                libusb_rc = 0;

            } else {

                if( (libusb_rc = libusb_get_string_descriptor_ascii(handle, pDesc->iSerialNumber, ((uint8_t*) mxId), sizeof(mxId))) < 0){
                    mvLog(MVLOG_WARN, "Failed to get string descriptor");

                    // retry
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }

                // Indicate no error
                libusb_rc = 0;

            }

        } while (libusb_rc != 0 && std::chrono::steady_clock::now() - t1 < RETRY_TIMEOUT);

        // Close opened device
        libusb_close(handle);

        // if mx_id couldn't be retrieved, exit by returning error
        if(libusb_rc != 0){
            return (libusb_error) libusb_rc;
        }

        // Cache the retrieved mx_id
        // Find empty space and store this entry
        // If no empty space, don't cache (possible case: >16 devices)
        int cache_index = usb_mx_id_cache_store_entry(mxId, devicePath.c_str());
        if(cache_index >= 0){
            // debug print
            mvLog(MVLOG_DEBUG, "Cached MX ID %s at index %d", mxId, cache_index);
        } else {
            // debug print
            mvLog(MVLOG_DEBUG, "Couldn't cache MX ID %s", mxId);
        }

    }

    outMxId = std::string(mxId);
    return libusb_error::LIBUSB_SUCCESS;

}

const char* xlink_libusb_strerror(int x) {
    return libusb_strerror((libusb_error) x);
}


static libusb_device_handle *usb_open_device(libusb_device *dev, uint8_t* endpoint)
{
    struct libusb_config_descriptor *cdesc;
    const struct libusb_interface_descriptor *ifdesc;
    libusb_device_handle *h = NULL;
    int res, i;

    if((res = libusb_open(dev, &h)) < 0)
    {
        //snprintf(err_string_buff, err_max_len, "cannot open device: %s\n", xlink_libusb_strerror(res));
        return 0;
    }

    // Get configuration first
    int active_configuration = -1;
    if((res = libusb_get_configuration(h, &active_configuration)) < 0){
        //snprintf(err_string_buff, err_max_len, "setting config 1 failed: %s\n", xlink_libusb_strerror(res));
        libusb_close(h);
        return 0;
    }

    // Check if set configuration call is needed
    if(active_configuration != 1){
        mvLog(MVLOG_DEBUG, "Setting configuration from %d to 1\n", active_configuration);
        if ((res = libusb_set_configuration(h, 1)) < 0) {
            mvLog(MVLOG_ERROR, "libusb_set_configuration: %s\n", xlink_libusb_strerror(res));
            //snprintf(err_string_buff, err_max_len, "setting config 1 failed: %s\n", xlink_libusb_strerror(res));
            libusb_close(h);
            return 0;
        }
    }

    if((res = libusb_claim_interface(h, 0)) < 0)
    {
        //snprintf(err_string_buff, err_max_len, "claiming interface 0 failed: %s\n", xlink_libusb_strerror(res));
        libusb_close(h);
        return 0;
    }
    if((res = libusb_get_config_descriptor(dev, 0, &cdesc)) < 0)
    {
        //snprintf(err_string_buff, err_max_len, "Unable to get USB config descriptor: %s\n", xlink_libusb_strerror(res));
        libusb_close(h);
        return 0;
    }
    ifdesc = cdesc->interface->altsetting;
    for(i=0; i<ifdesc->bNumEndpoints; i++)
    {
        mvLog(MVLOG_DEBUG, "Found EP 0x%02x : max packet size is %u bytes",
              ifdesc->endpoint[i].bEndpointAddress, ifdesc->endpoint[i].wMaxPacketSize);
        if((ifdesc->endpoint[i].bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
            continue;
        if( !(ifdesc->endpoint[i].bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) )
        {
            *endpoint = ifdesc->endpoint[i].bEndpointAddress;
            bulk_chunklen = ifdesc->endpoint[i].wMaxPacketSize;
            libusb_free_config_descriptor(cdesc);
            return h;
        }
    }
    libusb_free_config_descriptor(cdesc);
    libusb_close(h);
    return 0;
}

static int send_file(libusb_device_handle* h, uint8_t endpoint, const void* tx_buf, unsigned filesize,uint16_t bcdusb)
{
    using namespace std::chrono;

    uint8_t *p;
    int rc;
    int wb, twb, wbr;
    int bulk_chunklen = DEFAULT_CHUNKSZ;
    twb = 0;
    p = const_cast<uint8_t*>((const uint8_t*)tx_buf);
    int send_zlp = ((filesize % 512) == 0);

    if(bcdusb < 0x200) {
        bulk_chunklen = USB1_CHUNKSZ;
    }

    auto t1 = steady_clock::now();
    mvLog(MVLOG_DEBUG, "Performing bulk write of %u bytes...", filesize);
    while(((unsigned)twb < filesize) || send_zlp)
    {
        wb = filesize - twb;
        if(wb > bulk_chunklen)
            wb = bulk_chunklen;
        wbr = 0;
        rc = libusb_bulk_transfer(h, endpoint, p, wb, &wbr, write_timeout);
        if((rc || (wb != wbr)) && (wb != 0)) // Don't check the return code for ZLP
        {
            if(rc == LIBUSB_ERROR_NO_DEVICE)
                break;
            mvLog(MVLOG_WARN, "bulk write: %s (%d bytes written, %d bytes to write)", xlink_libusb_strerror(rc), wbr, wb);
            if(rc == LIBUSB_ERROR_TIMEOUT)
                return USB_BOOT_TIMEOUT;
            else return USB_BOOT_ERROR;
        }
        if (steady_clock::now() - t1 > DEFAULT_SEND_FILE_TIMEOUT) {
            return USB_BOOT_TIMEOUT;
        }
        if(wb == 0) // ZLP just sent, last packet
            break;
        twb += wbr;
        p += wbr;
    }

#ifndef NDEBUG
    double MBpS = ((double)filesize / 1048576.) / (duration_cast<duration<float>>(steady_clock::now() - t1)).count();
    mvLog(MVLOG_DEBUG, "Successfully sent %u bytes of data in %lf ms (%lf MB/s)", filesize, duration_cast<milliseconds>(steady_clock::now() - t1).count(), MBpS);
#endif

    return 0;
}

int usb_boot(const char *addr, const void *mvcmd, unsigned size)
{
    using namespace std::chrono;

    int rc = 0;
    uint8_t endpoint;

    libusb_device *dev = nullptr;
    libusb_device_handle *h;
    uint16_t bcdusb=-1;


    auto t1 = steady_clock::now();
    do {
        if(refLibusbDeviceByName(addr, &dev) == X_LINK_PLATFORM_SUCCESS){
            break;
        }
        std::this_thread::sleep_for(milliseconds(10));
    } while(steady_clock::now() - t1 < DEFAULT_CONNECT_TIMEOUT);

    if(dev == nullptr) {
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }

    auto t2 = steady_clock::now();
    do {
        if((h = usb_open_device(dev, &endpoint)) != nullptr){
            break;
        }
        std::this_thread::sleep_for(milliseconds(100));
    } while(steady_clock::now() - t2 < DEFAULT_CONNECT_TIMEOUT);

    if (h) {
        rc = send_file(h, endpoint, mvcmd, size, bcdusb);
        libusb_release_interface(h, 0);
        libusb_close(h);
    } else {
        rc = X_LINK_PLATFORM_INSUFFICIENT_PERMISSIONS;
    }

    if (dev) {
        libusb_unref_device(dev);
    }

    return rc;
}



libusb_device_handle *usbLinkOpen(const char *path)
{
    using namespace std::chrono;
    if (path == NULL) {
        return 0;
    }

    usbBootError_t rc = USB_BOOT_DEVICE_NOT_FOUND;
    libusb_device_handle *h = nullptr;
    libusb_device *dev = nullptr;
    bool found = false;

    auto t1 = steady_clock::now();
    do {
        if(refLibusbDeviceByName(path, &dev) == X_LINK_PLATFORM_SUCCESS){
            found = true;
            break;
        }
    } while(steady_clock::now() - t1 < DEFAULT_OPEN_TIMEOUT);

    if(!found) {
        return nullptr;
    }

    //usb_speed_enum = libusb_get_device_speed(dev);

    int libusb_rc = libusb_open(dev, &h);
    if (libusb_rc < 0)
    {
        libusb_unref_device(dev);
        return 0;
    }

    libusb_unref_device(dev);
    libusb_detach_kernel_driver(h, 0);
    libusb_rc = libusb_claim_interface(h, 0);
    if(libusb_rc < 0)
    {
        libusb_close(h);
        return 0;
    }

    return h;
}


bool usbLinkBootBootloader(const char *path) {

    libusb_device *dev = nullptr;
    refLibusbDeviceByName(path, &dev);
    if(dev == NULL){
        return 0;
    }
    libusb_device_handle *h = NULL;


    int libusb_rc = libusb_open(dev, &h);
    if (libusb_rc < 0)
    {
        libusb_unref_device(dev);
        return 0;
    }

    // Make control transfer
    libusb_control_transfer(h,
        bootBootloaderPacket.requestType,   // bmRequestType: device-directed
        bootBootloaderPacket.request,   // bRequest: custom
        bootBootloaderPacket.value, // wValue: custom
        bootBootloaderPacket.index, // wIndex
        NULL,   // data pointer
        0,      // data size
        1000    // timeout [ms]
    );

    // Ignore error and close device
    libusb_unref_device(dev);
    libusb_close(h);

    return true;

}

void usbLinkClose(libusb_device_handle *f)
{
    libusb_release_interface(f, 0);
    libusb_close(f);
}



int usbPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd)
{
#if (!defined(USE_USB_VSC))
    #ifdef USE_LINK_JTAG
    struct sockaddr_in serv_addr;
    usbFdWrite = socket(AF_INET, SOCK_STREAM, 0);
    usbFdRead = socket(AF_INET, SOCK_STREAM, 0);
    assert(usbFdWrite >=0);
    assert(usbFdRead >=0);
    memset(&serv_addr, '0', sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serv_addr.sin_port = htons(USB_LINK_SOCKET_PORT);

    if (connect(usbFdWrite, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        exit(1);
    }
    return 0;

#else
    usbFdRead= open(devPathRead, O_RDWR);
    if(usbFdRead < 0)
    {
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }
    // set tty to raw mode
    struct termios  tty;
    speed_t     spd;
    int rc;
    rc = tcgetattr(usbFdRead, &tty);
    if (rc < 0) {
        close(usbFdRead);
        usbFdRead = -1;
        return X_LINK_PLATFORM_ERROR;
    }

    spd = B115200;
    cfsetospeed(&tty, (speed_t)spd);
    cfsetispeed(&tty, (speed_t)spd);

    cfmakeraw(&tty);

    rc = tcsetattr(usbFdRead, TCSANOW, &tty);
    if (rc < 0) {
        close(usbFdRead);
        usbFdRead = -1;
        return X_LINK_PLATFORM_ERROR;
    }

    usbFdWrite= open(devPathWrite, O_RDWR);
    if(usbFdWrite < 0)
    {
        close(usbFdRead);
        usbFdWrite = -1;
        return X_LINK_PLATFORM_ERROR;
    }
    // set tty to raw mode
    rc = tcgetattr(usbFdWrite, &tty);
    if (rc < 0) {
        close(usbFdRead);
        close(usbFdWrite);
        usbFdWrite = -1;
        return X_LINK_PLATFORM_ERROR;
    }

    spd = B115200;
    cfsetospeed(&tty, (speed_t)spd);
    cfsetispeed(&tty, (speed_t)spd);

    cfmakeraw(&tty);

    rc = tcsetattr(usbFdWrite, TCSANOW, &tty);
    if (rc < 0) {
        close(usbFdRead);
        close(usbFdWrite);
        usbFdWrite = -1;
        return X_LINK_PLATFORM_ERROR;
    }
    return 0;
#endif  /*USE_LINK_JTAG*/
#else
    *fd = usbLinkOpen(devPathWrite);
    if (*fd == 0)
    {
        /* could fail due to port name change */
        return -1;
    }

    if(*fd)
        return 0;
    else
        return -1;
#endif  /*USE_USB_VSC*/
}


int usbPlatformClose(void *fd)
{

#ifndef USE_USB_VSC
    #ifdef USE_LINK_JTAG
    /*Nothing*/
#else
    if (usbFdRead != -1){
        close(usbFdRead);
        usbFdRead = -1;
    }
    if (usbFdWrite != -1){
        close(usbFdWrite);
        usbFdWrite = -1;
    }
#endif  /*USE_LINK_JTAG*/
#else
    usbLinkClose((libusb_device_handle *) fd);
#endif  /*USE_USB_VSC*/
    return -1;
}



int usbPlatformBootFirmware(const deviceDesc_t* deviceDesc, const char* firmware, size_t length){

    // Boot it
    int rc = usb_boot(deviceDesc->name, firmware, (unsigned)length);

    if(!rc) {
        mvLog(MVLOG_DEBUG, "Boot successful, device address %s", deviceDesc->name);
    }
    return rc;
}



int usb_read(libusb_device_handle *f, void *data, size_t size)
{
    const int chunk_size = DEFAULT_CHUNKSZ;
    while(size > 0)
    {
        int bt, ss = (int)size;
        if(ss > chunk_size)
            ss = chunk_size;
        int rc = libusb_bulk_transfer(f, USB_ENDPOINT_IN,(unsigned char *)data, ss, &bt, XLINK_USB_DATA_TIMEOUT);
        if(rc)
            return rc;
        data = ((char *)data) + bt;
        size -= bt;
    }
    return 0;
}

int usb_write(libusb_device_handle *f, const void *data, size_t size)
{
    const int chunk_size = DEFAULT_CHUNKSZ;
    while(size > 0)
    {
        int bt, ss = (int)size;
        if(ss > chunk_size)
            ss = chunk_size;
        int rc = libusb_bulk_transfer(f, USB_ENDPOINT_OUT, (unsigned char *)data, ss, &bt, XLINK_USB_DATA_TIMEOUT);
        if(rc)
            return rc;
        data = (char *)data + bt;
        size -= bt;
    }
    return 0;
}

int usbPlatformRead(void* fd, void* data, int size)
{
    int rc = 0;
#ifndef USE_USB_VSC
    int nread =  0;
#ifdef USE_LINK_JTAG
    while (nread < size){
        nread += read(usbFdWrite, &((char*)data)[nread], size - nread);
        printf("read %d %d\n", nread, size);
    }
#else
    if(usbFdRead < 0)
    {
        return -1;
    }

    while(nread < size)
    {
        int toRead = (PACKET_LENGTH && (size - nread > PACKET_LENGTH)) \
                        ? PACKET_LENGTH : size - nread;

        while(toRead > 0)
        {
            rc = read(usbFdRead, &((char*)data)[nread], toRead);
            if ( rc < 0)
            {
                return -2;
            }
            toRead -=rc;
            nread += rc;
        }
        unsigned char acknowledge = 0xEF;
        int wc = write(usbFdRead, &acknowledge, sizeof(acknowledge));
        if (wc != sizeof(acknowledge))
        {
            return -2;
        }
    }
#endif  /*USE_LINK_JTAG*/
#else
    rc = usb_read((libusb_device_handle *) fd, data, size);
#endif  /*USE_USB_VSC*/
    return rc;
}

int usbPlatformWrite(void *fd, void *data, int size)
{
    int rc = 0;
#ifndef USE_USB_VSC
    int byteCount = 0;
#ifdef USE_LINK_JTAG
    while (byteCount < size){
        byteCount += write(usbFdWrite, &((char*)data)[byteCount], size - byteCount);
        printf("write %d %d\n", byteCount, size);
    }
#else
    if(usbFdWrite < 0)
    {
        return -1;
    }
    while(byteCount < size)
    {
       int toWrite = (PACKET_LENGTH && (size - byteCount > PACKET_LENGTH)) \
                        ? PACKET_LENGTH:size - byteCount;
       int wc = write(usbFdWrite, ((char*)data) + byteCount, toWrite);

       if ( wc != toWrite)
       {
           return -2;
       }

       byteCount += toWrite;
       unsigned char acknowledge;
       int rc;
       rc = read(usbFdWrite, &acknowledge, sizeof(acknowledge));

       if ( rc < 0)
       {
           return -2;
       }

       if (acknowledge != 0xEF)
       {
           return -2;
       }
    }
#endif  /*USE_LINK_JTAG*/
#else
    rc = usb_write((libusb_device_handle *) fd, data, size);
#endif  /*USE_USB_VSC*/
    return rc;
}