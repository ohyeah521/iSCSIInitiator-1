/*!
 * @author		Nareg Sinenian
 * @file		iSCSIIORegistry.c
 * @version		1.0
 * @copyright	(c) 2013-2015 Nareg Sinenian. All rights reserved.
 * @brief		User-space management of iSCSI I/O registry entries.
 */

#include "iSCSIIORegistry.h"

#include <IOKit/IOKitLib.h>

// The following are included for definitions of various IORegistry constants
#include <IOKit/scsi/SCSITaskLib.h>
#include <IOKit/storage/IOStorageProtocolCharacteristics.h>
#include <IOKit/IOTypes.h>
#include <IOKit/IOBSD.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOBlockStorageDriver.h>


/*! Gets the iSCSIVirtualHBA object in the IO registry.*/
io_object_t iSCSIIORegistryGetiSCSIHBAEntry()
{
    // Create a dictionary to match iSCSIkext
    CFMutableDictionaryRef matchingDict = NULL;
    matchingDict = IOServiceMatching("com_NSinenian_iSCSIVirtualHBA");
    
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,matchingDict);
    
    return service;
}

/*! Finds the target object (IOSCSIParallelInterfaceDevice) in the IO registry that
 *  corresponds to the specified target.
 *  @param targetIQN the name of the target.
 *  @return the IO registry object of the IOSCSITargetDevice for this session. */
io_object_t iSCSIIORegistryGetTargetEntry(CFStringRef targetIQN)
{
    if(!targetIQN)
        return IO_OBJECT_NULL;
    
    io_service_t service;
    if(!(service = iSCSIIORegistryGetiSCSIHBAEntry()))
        return IO_OBJECT_NULL;
    
    // Iterate over the targets and find the specified target by name
    io_iterator_t iterator = IO_OBJECT_NULL;
    IORegistryEntryGetChildIterator(service,kIOServicePlane,&iterator);
    
    io_object_t entry;
    while((entry = IOIteratorNext(iterator)) != IO_OBJECT_NULL)
    {
        CFDictionaryRef protocolDict = IORegistryEntryCreateCFProperty(
            entry,CFSTR(kIOPropertyProtocolCharacteristicsKey),kCFAllocatorDefault,0);
        
        if(protocolDict)
        {
            CFStringRef IQN = CFDictionaryGetValue(protocolDict,CFSTR(kIOPropertyiSCSIQualifiedNameKey));
            if(CFStringCompare(IQN,targetIQN,0) == kCFCompareEqualTo)
            {
                CFRelease(protocolDict);
                IOObjectRelease(iterator);
                return entry;
            }
            CFRelease(protocolDict);
        }
        IOObjectRelease(entry);
    }
    
    IOObjectRelease(iterator);
    return IO_OBJECT_NULL;
}

/*! Gets an iterator for traversing iSCSI targets in the I/O registry.
 *  @param iterator the iterator.
 *  @return a kernel error code indicating the result of the operation. */
kern_return_t iSCSIIORegistryGetTargets(io_iterator_t * iterator)
{
    if(!iterator)
        return kIOReturnBadArgument;
    
    io_object_t service = iSCSIIORegistryGetiSCSIHBAEntry();
    
    if(service == IO_OBJECT_NULL)
        return kIOReturnNotFound;

    // The children of the iSCSI HBA are the targets (IOSCSIParallelInterfaceDevice)
    IORegistryEntryGetChildIterator(service,kIOServicePlane,iterator);
    
    return kIOReturnSuccess;
}

/*! Gets an iterator for traversing iSCSI LUNs for a specified target in the
 *  I/O registry.
 *  @param targetIQN the name of the target.
 *  @param iterator the iterator used to traverse LUNs for the specified target.
 *  @return a kernel error code indicating the result of the operation. */
kern_return_t iSCSIIORegistryGetLUNs(CFStringRef targetIQN,io_iterator_t * iterator)
{
    if(!iterator)
        return kIOReturnBadArgument;
    
    io_object_t parallelDevice = iSCSIIORegistryGetTargetEntry(targetIQN);
    
    if(parallelDevice == IO_OBJECT_NULL)
        return kIOReturnNotFound;
    
    // The children of the IOSCSIParallelInterfaceDevice are IOSCSITargetDevices
    io_object_t target;
    IORegistryEntryGetChildEntry(parallelDevice,kIOServicePlane,&target);

    if(target == IO_OBJECT_NULL) {
        IOObjectRelease(parallelDevice);
        return kIOReturnNotFound;
    }
    
    // The children of the target (IOSCSITargetDevice) are the LUNs
    kern_return_t result = IORegistryEntryGetChildIterator(target,kIOServicePlane,iterator);
    
    IOObjectRelease(parallelDevice);
    IOObjectRelease(target);
    
    return result;
}

/*! Applies a callback function all IOMedia objects of a particular target.
 *  @param target search children of this node for IOMedia objects.
 *  @param callback the callback function to call on each IOMedia object.
 *  @param context a user-defined parameter to pass to the callback function. */
void iSCSIIORegistryIOMediaApplyFunction(io_object_t target,
                                         iSCSIIOMediaCallback callback,
                                         void * context)
{
    io_object_t entry = IO_OBJECT_NULL;
    io_iterator_t iterator = IO_OBJECT_NULL;
    IORegistryEntryGetChildIterator(target,kIOServicePlane,&iterator);
    
    // Iterate over all children of the target object
    while((entry = IOIteratorNext(iterator)) != IO_OBJECT_NULL)
    {
        // Recursively call this function for each child of the target
        iSCSIIORegistryIOMediaApplyFunction(entry,callback,context);
        
        // Find the IOMedia's root provider class (IOBlockStorageDriver) and
        // get the first child.  This ensures that we grab the IOMedia object
        // for the disk itself and not each individual partition
        CFStringRef providerClass = IORegistryEntryCreateCFProperty(entry,CFSTR(kIOClassKey),kCFAllocatorDefault,0);

        if(providerClass && CFStringCompare(providerClass,CFSTR(kIOBlockStorageDriverClass),0) == kCFCompareEqualTo)
        {
            // Apply callback function to the child (the child is the the
            // IOMedia object that pertains to the whole disk)
            io_object_t child;
            IORegistryEntryGetChildEntry(entry,kIOServicePlane,&child);
            callback(child,context);
            IOObjectRelease(child);
        }
        
        if(providerClass)
            CFRelease(providerClass);
        
        IOObjectRelease(entry);
    }
    
    IOObjectRelease(iterator);
}

/*! Finds the IOMedia object associated with the LUN object.
 *  @param LUN the IO registry object corresponding to a LUN.
 *  @return the IOMedia object for the LUN. */
io_object_t iSCSIIORegistryFindIOMediaForLUN(io_object_t LUN)
{
    io_object_t entry = IO_OBJECT_NULL;
    IORegistryEntryGetChildEntry(LUN,kIOServicePlane,&entry);
    
    // Descend down the tree and find the first IOMedia class
    while(entry != IO_OBJECT_NULL)
    {
        CFStringRef class = IOObjectCopyClass(entry);
        
        if(class && CFStringCompare(class,CFSTR(kIOMediaClass),0) == kCFCompareEqualTo)
            return entry;
        
        if(class)
            CFRelease(class);
        
        // Descend down into the tree next time IOIteratorNext() is called
        io_object_t child;
        IORegistryEntryGetChildEntry(entry,kIOServicePlane,&child);
        IOObjectRelease(entry);
        entry = child;
    }
    
    return IO_OBJECT_NULL;
}

/*! Creates a dictionary of properties associated with the target.  These
 *  include the following keys:
 *
 *  kIOPropertySCSIVendorIdentification (CFStringRef)
 *  kIOPropertySCSIProductIdentification (CFStringRef)
 *  kIOPropertyiSCSIQualifiedNameKey (CFStringRef)
 *  kIOPropertySCSITargetIdentifierKey (CFNumberRef)
 *
 *  @param target the target IO registry object.
 *  @return a dictionary of values for the properties, or NULL if the object
 *  could not be found. */
CFDictionaryRef iSCSIIORegistryCreateCFPropertiesForTarget(io_object_t target)
{
    // Get the IOSCSITargetDevice object (child of IOParallelInterfaceDevice or "target")
    io_object_t child;
    IORegistryEntryGetChildEntry(target,kIOServicePlane,&child);
    
    if(child == IO_OBJECT_NULL)
        return NULL;

    CFStringRef vendor = IORegistryEntryCreateCFProperty(
        child,CFSTR(kIOPropertySCSIVendorIdentification),kCFAllocatorDefault,0);

    CFStringRef product = IORegistryEntryCreateCFProperty(
        child,CFSTR(kIOPropertySCSIProductIdentification),kCFAllocatorDefault,0);
    
    CFDictionaryRef protocolDict = IORegistryEntryCreateCFProperty(
        child,CFSTR(kIOPropertyProtocolCharacteristicsKey),kCFAllocatorDefault,0);

    CFStringRef targetIQN = CFDictionaryGetValue(protocolDict,CFSTR(kIOPropertyiSCSIQualifiedNameKey));
    CFNumberRef targetId = CFDictionaryGetValue(protocolDict,CFSTR(kIOPropertySCSITargetIdentifierKey));
    
    const void * keys[] = {
        CFSTR(kIOPropertySCSIVendorIdentification),
        CFSTR(kIOPropertySCSIProductIdentification),
        CFSTR(kIOPropertySCSITargetIdentifierKey),
        CFSTR(kIOPropertyiSCSIQualifiedNameKey)};
    
    const void * values[] = {
        vendor,
        product,
        targetId,
        targetIQN
    };
    
    CFDictionaryRef propertiesDict = CFDictionaryCreate(kCFAllocatorDefault,
                                                        keys,values,
                                                        sizeof(values)/sizeof(void*),
                                                        &kCFTypeDictionaryKeyCallBacks,
                                                        &kCFTypeDictionaryValueCallBacks);
    CFRelease(vendor);
    CFRelease(product);
    CFRelease(protocolDict);

    return propertiesDict;
}

/*! Creates a dictionary of properties associated with the LUN.  These
 *  include the following keys:
 *
 *  kIOPropertySCSILogicalUnitNumberKey (CFNumberRef)
 *  kIOPropertySCSIPeripheralDeviceType (CFNumberRef)
 *
 *  @param LUN the target IO registry object.
 *  @return a dictionary of values for the properties, or NULL if the object
 *  could not be found. */
CFDictionaryRef iSCSIIORegistryCreateCFPropertiesForLUN(io_object_t LUN)
{
    if(LUN == IO_OBJECT_NULL)
        return NULL;
    
    CFMutableDictionaryRef propertiesDict;
    IORegistryEntryCreateCFProperties(LUN,&propertiesDict,kCFAllocatorDefault,0);
    
    return propertiesDict;
}

/*! Creates a dictionary of properties associated with the LUN.  These
 *  include the following keys:
 *
 *  kIOBSDNameKey (CFStringRef)
 *  kIOMediaSizeKey (CFNumberRef)
 *  kIOMediaPreferredBlockSizeKey (CFNumberRef)
 *
 *  @param IOMedia the IOMedia IO registry object.
 *  @return a dictionary of values for the properties, or NULL if the object
 *  could not be found. */
CFDictionaryRef iSCSIIORegistryCreateCFPropertiesForIOMedia(io_object_t IOMedia)
{
    if(IOMedia == IO_OBJECT_NULL)
        return NULL;
    
    CFMutableDictionaryRef propertiesDict;
    IORegistryEntryCreateCFProperties(IOMedia,&propertiesDict,kCFAllocatorDefault,0);
    IOObjectRelease(IOMedia);
    
    return propertiesDict;
}

