/* $Id$ */
/** @file
 * Unattended class implementation
 */

/*
 * Copyright (C) 2006-2017 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_MAIN_UNATTENDED
#include "LoggingNew.h"
#include "VirtualBoxBase.h"
#include "UnattendedImpl.h"
#include "UnattendedInstaller.h"
#include "UnattendedScript.h"
#include "VirtualBoxImpl.h"
#include "SystemPropertiesImpl.h"
#include "MachineImpl.h"
#include "Global.h"

#include <VBox/err.h>
#include <iprt/ctype.h>
#include <iprt/file.h>
#include <iprt/locale.h>
#include <iprt/path.h>

using namespace std;


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/**
 * Controller slot for a DVD drive.
 *
 * The slot can be free and needing a drive to be attached along with the ISO
 * image, or it may already be there and only need mounting the ISO.  The
 * ControllerSlot::fFree member indicates which it is.
 */
struct ControllerSlot
{
    StorageBus_T    enmBus;
    Utf8Str         strControllerName;
    ULONG           uPort;
    ULONG           uDevice;
    bool            fFree;

    ControllerSlot(StorageBus_T a_enmBus, const Utf8Str &a_rName, ULONG a_uPort, ULONG a_uDevice, bool a_fFree)
        : enmBus(a_enmBus), strControllerName(a_rName), uPort(a_uPort), uDevice(a_uDevice), fFree(a_fFree)
    {}

    bool operator<(const ControllerSlot &rThat) const
    {
        if (enmBus == rThat.enmBus)
        {
            if (strControllerName == rThat.strControllerName)
            {
                if (uPort == rThat.uPort)
                    return uDevice < rThat.uDevice;
                return uPort < rThat.uPort;
            }
            return strControllerName < rThat.strControllerName;
        }

        /*
         * Bus comparsion in boot priority order.
         */
        /* IDE first. */
        if (enmBus == StorageBus_IDE)
            return true;
        if (rThat.enmBus == StorageBus_IDE)
            return false;
        /* SATA next */
        if (enmBus == StorageBus_SATA)
            return true;
        if (rThat.enmBus == StorageBus_SATA)
            return false;
        /* SCSI next */
        if (enmBus == StorageBus_SCSI)
            return true;
        if (rThat.enmBus == StorageBus_SCSI)
            return false;
        /* numerical */
        return (int)enmBus < (int)rThat.enmBus;
    }

    bool operator==(const ControllerSlot &rThat) const
    {
        return enmBus            == rThat.enmBus
            && strControllerName == rThat.strControllerName
            && uPort             == rThat.uPort
            && uDevice           == rThat.uDevice;
    }
};

/**
 * Installation disk.
 *
 * Used when reconfiguring the VM.
 */
typedef struct UnattendedInstallationDisk
{
    StorageBus_T    enmBusType;         /**< @todo nobody is using this... */
    Utf8Str         strControllerName;
    DeviceType_T    enmDeviceType;
    AccessMode_T    enmAccessType;
    ULONG           uPort;
    ULONG           uDevice;
    bool            fMountOnly;
    Utf8Str         strImagePath;

    UnattendedInstallationDisk(StorageBus_T a_enmBusType, Utf8Str const &a_rBusName, DeviceType_T a_enmDeviceType,
                               AccessMode_T a_enmAccessType, ULONG a_uPort, ULONG a_uDevice, bool a_fMountOnly,
                               Utf8Str const &a_rImagePath)
        : enmBusType(a_enmBusType), strControllerName(a_rBusName), enmDeviceType(a_enmDeviceType), enmAccessType(a_enmAccessType)
        , uPort(a_uPort), uDevice(a_uDevice), fMountOnly(a_fMountOnly), strImagePath(a_rImagePath)
    {
        Assert(strControllerName.length() > 0);
    }

    UnattendedInstallationDisk(std::list<ControllerSlot>::const_iterator const &itDvdSlot, Utf8Str const &a_rImagePath)
        : enmBusType(itDvdSlot->enmBus), strControllerName(itDvdSlot->strControllerName), enmDeviceType(DeviceType_DVD)
        , enmAccessType(AccessMode_ReadOnly), uPort(itDvdSlot->uPort), uDevice(itDvdSlot->uDevice)
        , fMountOnly(!itDvdSlot->fFree), strImagePath(a_rImagePath)
    {
        Assert(strControllerName.length() > 0);
    }
} UnattendedInstallationDisk;


//////////////////////////////////////////////////////////////////////////////////////////////////////
/*
*
*
*  Implementation Unattended functions
*
*/
//////////////////////////////////////////////////////////////////////////////////////////////////////

Unattended::Unattended()
    : mhThreadReconfigureVM(NIL_RTNATIVETHREAD), mfRtcUseUtc(false), mfGuestOs64Bit(false)
    , mpInstaller(NULL), mpTimeZoneInfo(NULL), mfIsDefaultAuxiliaryBasePath(true)
{ }

Unattended::~Unattended()
{
    if (mpInstaller)
    {
        delete mpInstaller;
        mpInstaller = NULL;
    }
}

HRESULT Unattended::FinalConstruct()
{
    return BaseFinalConstruct();
}

void Unattended::FinalRelease()
{
    uninit();

    BaseFinalRelease();
}

void Unattended::uninit()
{
    /* Enclose the state transition Ready->InUninit->NotReady */
    AutoUninitSpan autoUninitSpan(this);
    if (autoUninitSpan.uninitDone())
        return;

    unconst(mParent) = NULL;
    mMachine.setNull();
}

/**
 * Initializes the unattended object.
 *
 * @param aParent  Pointer to the parent object.
 */
HRESULT Unattended::initUnattended(VirtualBox *aParent)
{
    LogFlowThisFunc(("aParent=%p\n", aParent));
    ComAssertRet(aParent, E_INVALIDARG);

    /* Enclose the state transition NotReady->InInit->Ready */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), E_FAIL);

    unconst(mParent) = aParent;

    /*
     * Fill public attributes (IUnattended) with useful defaults.
     */
    try
    {
        mStrUser                    = "vboxuser";
        mStrPassword                = "changeme";
        mfInstallGuestAdditions     = false;
        mfInstallTestExecService    = false;
        midxImage                   = 1;

        HRESULT hrc = mParent->i_getSystemProperties()->i_getDefaultAdditionsISO(mStrAdditionsIsoPath);
        ComAssertComRCRet(hrc, hrc);
    }
    catch (std::bad_alloc)
    {
        return E_OUTOFMEMORY;
    }

    /*
     * Confirm a successful initialization
     */
    autoInitSpan.setSucceeded();

    return S_OK;
}

HRESULT Unattended::detectIsoOS()
{
    return E_NOTIMPL;
}

HRESULT Unattended::prepare()
{
    LogFlow(("Unattended::prepare: enter\n"));

    /*
     * Must have a machine.
     */
    ComPtr<Machine> ptrMachine;
    Guid            MachineUuid;
    {
        AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
        ptrMachine = mMachine;
        if (ptrMachine.isNull())
            return setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("No machine associated with this IUnatteded instance"));
        MachineUuid = mMachineUuid;
    }

    /*
     * Before we write lock ourselves, we must get stuff from Machine and
     * VirtualBox because their locks have higher priorities than ours.
     */
    Utf8Str strGuestOsTypeId;
    Utf8Str strMachineName;
    Utf8Str strDefaultAuxBasePath;
    HRESULT hrc;
    try
    {
        Bstr bstrTmp;
        hrc = ptrMachine->COMGETTER(OSTypeId)(bstrTmp.asOutParam());
        if (SUCCEEDED(hrc))
        {
            strGuestOsTypeId = bstrTmp;
            hrc = ptrMachine->COMGETTER(Name)(bstrTmp.asOutParam());
            if (SUCCEEDED(hrc))
                strMachineName = bstrTmp;
        }
        int vrc = ptrMachine->i_calculateFullPath(Utf8StrFmt("Unattended-%RTuuid-", MachineUuid.raw()), strDefaultAuxBasePath);
        if (RT_FAILURE(vrc))
            return setErrorBoth(E_FAIL, vrc);
    }
    catch (std::bad_alloc)
    {
        return E_OUTOFMEMORY;
    }
    bool const fIs64Bit = i_isGuestOSArchX64(strGuestOsTypeId);

    BOOL fRtcUseUtc = FALSE;
    hrc = ptrMachine->COMGETTER(RTCUseUTC)(&fRtcUseUtc);
    if (FAILED(hrc))
        return hrc;

    /*
     * Write lock this object and set attributes we got from IMachine.
     */
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    mStrGuestOsTypeId = strGuestOsTypeId;
    mfGuestOs64Bit    = fIs64Bit;
    mfRtcUseUtc       = RT_BOOL(fRtcUseUtc);

    /*
     * Do some state checks.
     */
    if (mpInstaller != NULL)
        return setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("The prepare method has been called (must call done to restart)"));
    if ((Machine *)ptrMachine != (Machine *)mMachine)
        return setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("The 'machine' while we were using it - please don't do that"));

    /*
     * Check if the specified ISOs and files exist.
     */
    if (!RTFileExists(mStrIsoPath.c_str()))
        return setErrorBoth(E_FAIL, VERR_FILE_NOT_FOUND, tr("Could not locate the installation ISO file '%s'"),
                            mStrIsoPath.c_str());
    if (mfInstallGuestAdditions && !RTFileExists(mStrAdditionsIsoPath.c_str()))
        return setErrorBoth(E_FAIL, VERR_FILE_NOT_FOUND, tr("Could not locate the guest additions ISO file '%s'"),
                            mStrAdditionsIsoPath.c_str());
    if (mfInstallTestExecService && !RTFileExists(mStrValidationKitIsoPath.c_str()))
        return setErrorBoth(E_FAIL, VERR_FILE_NOT_FOUND, tr("Could not locate the validation kit ISO file '%s'"),
                            mStrValidationKitIsoPath.c_str());
    if (mStrScriptTemplatePath.isNotEmpty() && !RTFileExists(mStrScriptTemplatePath.c_str()))
        return setErrorBoth(E_FAIL, VERR_FILE_NOT_FOUND, tr("Could not locate unattended installation script template '%s'"),
                            mStrScriptTemplatePath.c_str());

    /*
     * Do some default property stuff and check other properties.
     */
    try
    {
        char szTmp[128];

        if (mStrLocale.isEmpty())
        {
            int vrc = RTLocaleQueryNormalizedBaseLocaleName(szTmp, sizeof(szTmp));
            if (   RT_SUCCESS(vrc)
                && RTLOCALE_IS_LANGUAGE2_UNDERSCORE_COUNTRY2(szTmp))
                mStrLocale.assign(szTmp, 5);
            else
                mStrLocale = "en_US";
            Assert(RTLOCALE_IS_LANGUAGE2_UNDERSCORE_COUNTRY2(mStrLocale));
        }

        if (mStrCountry.isEmpty())
        {
            int vrc = RTLocaleQueryUserCountryCode(szTmp);
            if (RT_SUCCESS(vrc))
                mStrCountry = szTmp;
            else if (   mStrLocale.isNotEmpty()
                     && RTLOCALE_IS_LANGUAGE2_UNDERSCORE_COUNTRY2(mStrLocale))
                mStrCountry.assign(mStrLocale, 3, 2);
            else
                mStrCountry = "US";
        }

        if (mStrTimeZone.isEmpty())
        {
            int vrc = RTTimeZoneGetCurrent(szTmp, sizeof(szTmp));
            if (RT_SUCCESS(vrc))
                mStrTimeZone = szTmp;
            else
                mStrTimeZone = "Etc/UTC";
            Assert(mStrTimeZone.isNotEmpty());
        }
        mpTimeZoneInfo = RTTimeZoneGetInfoByUnixName(mStrTimeZone.c_str());
        if (!mpTimeZoneInfo)
            mpTimeZoneInfo = RTTimeZoneGetInfoByWindowsName(mStrTimeZone.c_str());
        Assert(mpTimeZoneInfo || mStrTimeZone != "Etc/UTC");
        if (!mpTimeZoneInfo)
            LogRel(("Unattended::prepare: warning: Unknown time zone '%s'\n", mStrTimeZone.c_str()));

        if (mStrHostname.isEmpty())
        {
            /* Mangle the VM name into a valid hostname. */
            for (size_t i = 0; i < strMachineName.length(); i++)
            {
                char ch = strMachineName[i];
                if (   (unsigned)ch < 127
                    && RT_C_IS_ALNUM(ch))
                    mStrHostname.append(ch);
                else if (mStrHostname.isNotEmpty() && RT_C_IS_PUNCT(ch) && !mStrHostname.endsWith("-"))
                    mStrHostname.append('-');
            }
            if (mStrHostname.length() == 0)
                mStrHostname.printf("%RTuuid-vm", MachineUuid.raw());
            else if (mStrHostname.length() < 3)
                mStrHostname.append("-vm");
            mStrHostname.append(".myguest.virtualbox.org");
        }

        if (mStrAuxiliaryBasePath.isEmpty())
        {
            mStrAuxiliaryBasePath = strDefaultAuxBasePath;
            mfIsDefaultAuxiliaryBasePath = true;
        }
    }
    catch (std::bad_alloc)
    {
        return E_OUTOFMEMORY;
    }

    /*
     * Get the guest OS type info and instantiate the appropriate installer.
     */
    uint32_t   const idxOSType = Global::getOSTypeIndexFromId(mStrGuestOsTypeId.c_str());
    meGuestOsType     = idxOSType < Global::cOSTypes ? Global::sOSTypes[idxOSType].osType : VBOXOSTYPE_Unknown;

    mpInstaller = UnattendedInstaller::createInstance(meGuestOsType, mStrGuestOsTypeId, this);
    if (mpInstaller != NULL)
    {
        hrc = mpInstaller->initInstaller();
        if (SUCCEEDED(hrc))
        {
            /*
             * Do the script preps (just reads them).
             */
            hrc = mpInstaller->prepareUnattendedScripts();
            if (SUCCEEDED(hrc))
            {
                LogFlow(("Unattended::prepare: returns S_OK\n"));
                return S_OK;
            }
        }

        /* Destroy the installer instance. */
        delete mpInstaller;
        mpInstaller = NULL;
    }
    else
        hrc = setErrorBoth(E_FAIL, VERR_NOT_FOUND,
                           tr("Unattended installation is not supported for guest type '%s'"), mStrGuestOsTypeId.c_str());
    LogRelFlow(("Unattended::prepare: failed with %Rhrc\n", hrc));
    return hrc;
}

HRESULT Unattended::constructMedia()
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    LogFlow(("===========================================================\n"));
    LogFlow(("Call Unattended::constructMedia()\n"));

    if (mpInstaller == NULL)
        return setErrorBoth(E_FAIL, VERR_WRONG_ORDER, "prepare() not yet called");

    return mpInstaller->prepareMedia();
}

HRESULT Unattended::reconfigureVM()
{
    LogFlow(("===========================================================\n"));
    LogFlow(("Call Unattended::reconfigureVM()\n"));

    /*
     * Interrogate VirtualBox/IGuestOSType before we lock stuff and create ordering issues.
     */
    StorageBus_T enmRecommendedStorageBus = StorageBus_IDE;
    {
        Bstr bstrGuestOsTypeId;
        {
            AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
            bstrGuestOsTypeId = mStrGuestOsTypeId;
        }
        ComPtr<IGuestOSType> ptrGuestOSType;
        HRESULT hrc = mParent->GetGuestOSType(bstrGuestOsTypeId.raw(), ptrGuestOSType.asOutParam());
        if (SUCCEEDED(hrc))
            hrc = ptrGuestOSType->COMGETTER(RecommendedDVDStorageBus)(&enmRecommendedStorageBus);
        if (FAILED(hrc))
            return hrc;
    }

    /*
     * Take write lock (for lock order reasons, write lock our parent object too)
     * then make sure we're the only caller of this method.
     */
    AutoMultiWriteLock2 alock(mMachine, this COMMA_LOCKVAL_SRC_POS);
    HRESULT hrc;
    if (mhThreadReconfigureVM == NIL_RTNATIVETHREAD)
    {
        RTNATIVETHREAD const hNativeSelf = RTThreadNativeSelf();
        mhThreadReconfigureVM = hNativeSelf;

        /*
         * Create a new session, lock the machine and get the session machine object.
         * Do the locking without pinning down the write locks, just to be on the safe side.
         */
        ComPtr<ISession> ptrSession;
        try
        {
            hrc = ptrSession.createInprocObject(CLSID_Session);
        }
        catch (std::bad_alloc)
        {
            hrc = E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hrc))
        {
            alock.release();
            hrc = mMachine->LockMachine(ptrSession, LockType_Shared);
            alock.acquire();
            if (SUCCEEDED(hrc))
            {
                ComPtr<IMachine> ptrSessionMachine;
                hrc = ptrSession->COMGETTER(Machine)(ptrSessionMachine.asOutParam());
                if (SUCCEEDED(hrc))
                {
                    /*
                     * Hand the session to the inner work and let it do it job.
                     */
                    try
                    {
                        hrc = i_innerReconfigureVM(alock, enmRecommendedStorageBus, ptrSessionMachine);
                    }
                    catch (...)
                    {
                        hrc = E_UNEXPECTED;
                    }
                }

                /* Paranoia: release early in case we it a bump below.  */
                Assert(mhThreadReconfigureVM == hNativeSelf);
                mhThreadReconfigureVM = NIL_RTNATIVETHREAD;

                /*
                 * While unlocking the machine we'll have to drop the locks again.
                 */
                alock.release();

                ptrSessionMachine.setNull();
                HRESULT hrc2 = ptrSession->UnlockMachine();
                AssertLogRelMsg(SUCCEEDED(hrc2), ("UnlockMachine -> %Rhrc\n", hrc2));

                ptrSession.setNull();

                alock.acquire();
            }
            else
                mhThreadReconfigureVM = NIL_RTNATIVETHREAD;
        }
        else
            mhThreadReconfigureVM = NIL_RTNATIVETHREAD;
    }
    else
        hrc = setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("reconfigureVM running on other thread"));
    return hrc;
}


HRESULT Unattended::i_innerReconfigureVM(AutoMultiWriteLock2 &rAutoLock, StorageBus_T enmRecommendedStorageBus,
                                         ComPtr<IMachine> const &rPtrSessionMachine)
{
    if (mpInstaller == NULL)
        return setErrorBoth(E_FAIL, VERR_WRONG_ORDER, "prepare() not yet called");

    // Fetch all available storage controllers
    com::SafeIfaceArray<IStorageController> arrayOfControllers;
    HRESULT hrc = rPtrSessionMachine->COMGETTER(StorageControllers)(ComSafeArrayAsOutParam(arrayOfControllers));
    AssertComRCReturn(hrc, hrc);

    /*
     * Figure out where the images are to be mounted, adding controllers/ports as needed.
     */
    std::vector<UnattendedInstallationDisk> vecInstallationDisks;
    if (mpInstaller->isAuxiliaryFloppyNeeded())
    {
        hrc = i_reconfigureFloppy(arrayOfControllers, vecInstallationDisks, rPtrSessionMachine, rAutoLock);
        if (FAILED(hrc))
            return hrc;
    }

    hrc = i_reconfigureIsos(arrayOfControllers, vecInstallationDisks, rPtrSessionMachine, rAutoLock, enmRecommendedStorageBus);
    if (FAILED(hrc))
        return hrc;

    /*
     * Mount the images.
     */
    for (size_t idxImage = 0; idxImage < vecInstallationDisks.size(); idxImage++)
    {
        UnattendedInstallationDisk const *pImage = &vecInstallationDisks.at(idxImage);
        Assert(pImage->strImagePath.isNotEmpty());
        hrc = i_attachImage(pImage, rPtrSessionMachine, rAutoLock);
        if (FAILED(hrc))
            return hrc;
    }

    /*
     * Set the boot order.
     *
     * ASSUME that the HD isn't bootable when we start out, but it will be what
     * we boot from after the first stage of the installation is done.  Setting
     * it first prevents endless reboot cylces.
     */
    /** @todo consider making 100% sure the disk isn't bootable (edit partition
     *        table active bits and EFI stuff). */
    Assert(   mpInstaller->getBootableDeviceType() == DeviceType_DVD
           || mpInstaller->getBootableDeviceType() == DeviceType_Floppy);
    hrc = rPtrSessionMachine->SetBootOrder(1, DeviceType_HardDisk);
    if (SUCCEEDED(hrc))
        hrc = rPtrSessionMachine->SetBootOrder(2, mpInstaller->getBootableDeviceType());
    if (SUCCEEDED(hrc))
        hrc = rPtrSessionMachine->SetBootOrder(3, mpInstaller->getBootableDeviceType() == DeviceType_DVD
                                                  ? (DeviceType_T)DeviceType_Floppy : (DeviceType_T)DeviceType_DVD);
    if (FAILED(hrc))
        return hrc;

    /*
     * Essential step.
     *
     * HACK ALERT! We have to release the lock here or we'll get into trouble with
     *             the VirtualBox lock (via i_saveHardware/NetworkAdaptger::i_hasDefaults/VirtualBox::i_findGuestOSType).
     */
    if (SUCCEEDED(hrc))
    {
        rAutoLock.release();
        hrc = rPtrSessionMachine->SaveSettings();
        rAutoLock.acquire();
    }

    return hrc;
}

/**
 * Makes sure we've got a floppy drive attached to a floppy controller, adding
 * the auxiliary floppy image to the installation disk vector.
 *
 * @returns COM status code.
 * @param   rControllers            The existing controllers.
 * @param   rVecInstallatationDisks The list of image to mount.
 * @param   rPtrSessionMachine      The session machine smart pointer.
 * @param   rAutoLock               The lock.
 */
HRESULT Unattended::i_reconfigureFloppy(com::SafeIfaceArray<IStorageController> &rControllers,
                                        std::vector<UnattendedInstallationDisk> &rVecInstallatationDisks,
                                        ComPtr<IMachine> const &rPtrSessionMachine,
                                        AutoMultiWriteLock2 &rAutoLock)
{
    Assert(mpInstaller->isAuxiliaryFloppyNeeded());

    /*
     * Look for a floppy controller with a primary drive (A:) we can "insert"
     * the auxiliary floppy image.  Add a controller and/or a drive if necessary.
     */
    bool    fFoundPort0Dev0 = false;
    Bstr    bstrControllerName;
    Utf8Str strControllerName;

    for (size_t i = 0; i < rControllers.size(); ++i)
    {
        StorageBus_T enmStorageBus;
        HRESULT hrc = rControllers[i]->COMGETTER(Bus)(&enmStorageBus);
        AssertComRCReturn(hrc, hrc);
        if (enmStorageBus == StorageBus_Floppy)
        {

            /*
             * Found a floppy controller.
             */
            hrc = rControllers[i]->COMGETTER(Name)(bstrControllerName.asOutParam());
            AssertComRCReturn(hrc, hrc);

            /*
             * Check the attchments to see if we've got a device 0 attached on port 0.
             *
             * While we're at it we eject flppies from all floppy drives we encounter,
             * we don't want any confusion at boot or during installation.
             */
            com::SafeIfaceArray<IMediumAttachment> arrayOfMediumAttachments;
            hrc = rPtrSessionMachine->GetMediumAttachmentsOfController(bstrControllerName.raw(),
                                                                       ComSafeArrayAsOutParam(arrayOfMediumAttachments));
            AssertComRCReturn(hrc, hrc);
            strControllerName = bstrControllerName;
            AssertLogRelReturn(strControllerName.isNotEmpty(), setErrorBoth(E_UNEXPECTED, VERR_INTERNAL_ERROR_2));

            for (size_t j = 0; j < arrayOfMediumAttachments.size(); j++)
            {
                LONG iPort = -1;
                hrc = arrayOfMediumAttachments[j]->COMGETTER(Port)(&iPort);
                AssertComRCReturn(hrc, hrc);

                LONG iDevice = -1;
                hrc = arrayOfMediumAttachments[j]->COMGETTER(Device)(&iDevice);
                AssertComRCReturn(hrc, hrc);

                DeviceType_T enmType;
                hrc = arrayOfMediumAttachments[j]->COMGETTER(Type)(&enmType);
                AssertComRCReturn(hrc, hrc);

                if (enmType == DeviceType_Floppy)
                {
                    ComPtr<IMedium> ptrMedium;
                    hrc = arrayOfMediumAttachments[j]->COMGETTER(Medium)(ptrMedium.asOutParam());
                    AssertComRCReturn(hrc, hrc);

                    if (ptrMedium.isNotNull())
                    {
                        ptrMedium.setNull();
                        rAutoLock.release();
                        hrc = rPtrSessionMachine->UnmountMedium(bstrControllerName.raw(), iPort, iDevice, TRUE /*fForce*/);
                        rAutoLock.acquire();
                    }

                    if (iPort == 0 && iDevice == 0)
                        fFoundPort0Dev0 = true;
                }
                else if (iPort == 0 && iDevice == 0)
                    return setError(E_FAIL,
                                    tr("Found non-floppy device attached to port 0 device 0 on the floppy controller '%ls'"),
                                    bstrControllerName.raw());
            }
        }
    }

    /*
     * Add a floppy controller if we need to.
     */
    if (strControllerName.isEmpty())
    {
        bstrControllerName = strControllerName = "Floppy";
        ComPtr<IStorageController> ptrControllerIgnored;
        HRESULT hrc = rPtrSessionMachine->AddStorageController(bstrControllerName.raw(), StorageBus_Floppy,
                                                               ptrControllerIgnored.asOutParam());
        LogRelFunc(("Machine::addStorageController(Floppy) -> %Rhrc \n", hrc));
        if (FAILED(hrc))
            return hrc;
    }

    /*
     * Adding a floppy drive (if needed) and mounting the auxiliary image is
     * done later together with the ISOs.
     */
    rVecInstallatationDisks.push_back(UnattendedInstallationDisk(StorageBus_Floppy, strControllerName,
                                                                 DeviceType_Floppy, AccessMode_ReadWrite,
                                                                 0, 0,
                                                                 fFoundPort0Dev0 /*fMountOnly*/,
                                                                 mpInstaller->getAuxiliaryFloppyFilePath()));
    return S_OK;
}

/**
 * Reconfigures DVD drives of the VM to mount all the ISOs we need.
 *
 * This will umount all DVD media.
 *
 * @returns COM status code.
 * @param   rControllers            The existing controllers.
 * @param   rVecInstallatationDisks The list of image to mount.
 * @param   rPtrSessionMachine      The session machine smart pointer.
 * @param   rAutoLock               The lock.
 * @param   enmRecommendedStorageBus The recommended storage bus type for adding
 *                                   DVD drives on.
 */
HRESULT Unattended::i_reconfigureIsos(com::SafeIfaceArray<IStorageController> &rControllers,
                                      std::vector<UnattendedInstallationDisk> &rVecInstallatationDisks,
                                      ComPtr<IMachine> const &rPtrSessionMachine,
                                      AutoMultiWriteLock2 &rAutoLock, StorageBus_T enmRecommendedStorageBus)
{
    /*
     * Enumerate the attachements of every controller, looking for DVD drives,
     * ASSUMEING all drives are bootable.
     *
     * Eject the medium from all the drives (don't want any confusion) and look
     * for the recommended storage bus in case we need to add more drives.
     */
    HRESULT                    hrc;
    std::list<ControllerSlot>  lstControllerDvdSlots;
    Utf8Str                    strRecommendedControllerName; /* non-empty if recommended bus found. */
    Utf8Str                    strControllerName;
    Bstr                       bstrControllerName;
    for (size_t i = 0; i < rControllers.size(); ++i)
    {
        hrc = rControllers[i]->COMGETTER(Name)(bstrControllerName.asOutParam());
        AssertComRCReturn(hrc, hrc);
        strControllerName = bstrControllerName;

        /* Look for recommended storage bus. */
        StorageBus_T enmStorageBus;
        hrc = rControllers[i]->COMGETTER(Bus)(&enmStorageBus);
        AssertComRCReturn(hrc, hrc);
        if (enmStorageBus == enmRecommendedStorageBus)
        {
            strRecommendedControllerName = bstrControllerName;
            AssertLogRelReturn(strControllerName.isNotEmpty(), setErrorBoth(E_UNEXPECTED, VERR_INTERNAL_ERROR_2));
        }

        /* Scan the controller attachments. */
        com::SafeIfaceArray<IMediumAttachment> arrayOfMediumAttachments;
        hrc = rPtrSessionMachine->GetMediumAttachmentsOfController(bstrControllerName.raw(),
                                                                  ComSafeArrayAsOutParam(arrayOfMediumAttachments));
        AssertComRCReturn(hrc, hrc);

        for (size_t j = 0; j < arrayOfMediumAttachments.size(); j++)
        {
            DeviceType_T enmType;
            hrc = arrayOfMediumAttachments[j]->COMGETTER(Type)(&enmType);
            AssertComRCReturn(hrc, hrc);
            if (enmType == DeviceType_DVD)
            {
                LONG iPort = -1;
                hrc = arrayOfMediumAttachments[j]->COMGETTER(Port)(&iPort);
                AssertComRCReturn(hrc, hrc);

                LONG iDevice = -1;
                hrc = arrayOfMediumAttachments[j]->COMGETTER(Device)(&iDevice);
                AssertComRCReturn(hrc, hrc);

                /* Remeber it. */
                lstControllerDvdSlots.push_back(ControllerSlot(enmStorageBus, strControllerName, iPort, iDevice, false /*fFree*/));

                /* Eject the medium, if any. */
                ComPtr<IMedium> ptrMedium;
                hrc = arrayOfMediumAttachments[j]->COMGETTER(Medium)(ptrMedium.asOutParam());
                AssertComRCReturn(hrc, hrc);
                if (ptrMedium.isNotNull())
                {
                    ptrMedium.setNull();

                    rAutoLock.release();
                    hrc = rPtrSessionMachine->UnmountMedium(bstrControllerName.raw(), iPort, iDevice, TRUE /*fForce*/);
                    rAutoLock.acquire();
                }
            }
        }
    }

    /*
     * How many drives do we need? Add more if necessary.
     */
    ULONG cDvdDrivesNeeded = 0;
    if (mpInstaller->isAuxiliaryIsoNeeded())
        cDvdDrivesNeeded++;
    if (mpInstaller->isOriginalIsoNeeded())
        cDvdDrivesNeeded++;
#if 0 /* These are now in the AUX VISO. */
    if (mpInstaller->isAdditionsIsoNeeded())
        cDvdDrivesNeeded++;
    if (mpInstaller->isValidationKitIsoNeeded())
        cDvdDrivesNeeded++;
#endif
    Assert(cDvdDrivesNeeded > 0);
    if (cDvdDrivesNeeded > lstControllerDvdSlots.size())
    {
        /* Do we need to add the recommended controller? */
        if (strRecommendedControllerName.isEmpty())
        {
            switch (enmRecommendedStorageBus)
            {
                case StorageBus_IDE:    strRecommendedControllerName = "IDE";  break;
                case StorageBus_SATA:   strRecommendedControllerName = "SATA"; break;
                case StorageBus_SCSI:   strRecommendedControllerName = "SCSI"; break;
                case StorageBus_SAS:    strRecommendedControllerName = "SAS";  break;
                case StorageBus_USB:    strRecommendedControllerName = "USB";  break;
                case StorageBus_PCIe:   strRecommendedControllerName = "PCIe"; break;
                default:
                    return setError(E_FAIL, tr("Support for recommended storage bus %d not implemented"),
                                    (int)enmRecommendedStorageBus);
            }
            ComPtr<IStorageController> ptrControllerIgnored;
            hrc = rPtrSessionMachine->AddStorageController(Bstr(strRecommendedControllerName).raw(), enmRecommendedStorageBus,
                                                           ptrControllerIgnored.asOutParam());
            LogRelFunc(("Machine::addStorageController(%s) -> %Rhrc \n", strRecommendedControllerName.c_str(), hrc));
            if (FAILED(hrc))
                return hrc;
        }

        /* Add free controller slots, maybe raising the port limit on the controller if we can. */
        hrc = i_findOrCreateNeededFreeSlots(strRecommendedControllerName, enmRecommendedStorageBus, rPtrSessionMachine,
                                            cDvdDrivesNeeded, lstControllerDvdSlots);
        if (FAILED(hrc))
            return hrc;
        if (cDvdDrivesNeeded > lstControllerDvdSlots.size())
        {
            /* We could in many cases create another controller here, but it's not worth the effort. */
            return setError(E_FAIL, tr("Not enough free slots on controller '%s' to add %u DVD drive(s)"),
                            strRecommendedControllerName.c_str(), cDvdDrivesNeeded - lstControllerDvdSlots.size());
        }
        Assert(cDvdDrivesNeeded == lstControllerDvdSlots.size());
    }

    /*
     * Sort the DVD slots in boot order.
     */
    lstControllerDvdSlots.sort();

    /*
     * Prepare ISO mounts.
     *
     * Boot order depends on bootFromAuxiliaryIso() and we must grab DVD slots
     * according to the boot order.
     */
    std::list<ControllerSlot>::const_iterator itDvdSlot = lstControllerDvdSlots.begin();
    if (mpInstaller->isAuxiliaryIsoNeeded() && mpInstaller->bootFromAuxiliaryIso())
    {
        rVecInstallatationDisks.push_back(UnattendedInstallationDisk(itDvdSlot, mpInstaller->getAuxiliaryIsoFilePath()));
        ++itDvdSlot;
    }

    if (mpInstaller->isOriginalIsoNeeded())
    {
        rVecInstallatationDisks.push_back(UnattendedInstallationDisk(itDvdSlot, i_getIsoPath()));
        ++itDvdSlot;
    }

    if (mpInstaller->isAuxiliaryIsoNeeded() && !mpInstaller->bootFromAuxiliaryIso())
    {
        rVecInstallatationDisks.push_back(UnattendedInstallationDisk(itDvdSlot, mpInstaller->getAuxiliaryIsoFilePath()));
        ++itDvdSlot;
    }

#if 0 /* These are now in the AUX VISO. */
    if (mpInstaller->isAdditionsIsoNeeded())
    {
        rVecInstallatationDisks.push_back(UnattendedInstallationDisk(itDvdSlot, i_getAdditionsIsoPath()));
        ++itDvdSlot;
    }

    if (mpInstaller->isValidationKitIsoNeeded())
    {
        rVecInstallatationDisks.push_back(UnattendedInstallationDisk(itDvdSlot, i_getValidationKitIsoPath()));
        ++itDvdSlot;
    }
#endif

    return S_OK;
}

/**
 * Used to find more free slots for DVD drives during VM reconfiguration.
 *
 * This may modify the @a portCount property of the given controller.
 *
 * @returns COM status code.
 * @param   rStrControllerName      The name of the controller to find/create
 *                                  free slots on.
 * @param   enmStorageBus           The storage bus type.
 * @param   rPtrSessionMachine      Reference to the session machine.
 * @param   cSlotsNeeded            Total slots needed (including those we've
 *                                  already found).
 * @param   rDvdSlots               The slot collection for DVD drives to add
 *                                  free slots to as we find/create them.
 */
HRESULT Unattended::i_findOrCreateNeededFreeSlots(const Utf8Str &rStrControllerName, StorageBus_T enmStorageBus,
                                                  ComPtr<IMachine> const &rPtrSessionMachine, uint32_t cSlotsNeeded,
                                                  std::list<ControllerSlot> &rDvdSlots)
{
    Assert(cSlotsNeeded > rDvdSlots.size());

    /*
     * Get controlleer stats.
     */
    ComPtr<IStorageController> pController;
    HRESULT hrc = rPtrSessionMachine->GetStorageControllerByName(Bstr(rStrControllerName).raw(), pController.asOutParam());
    AssertComRCReturn(hrc, hrc);

    ULONG cMaxDevicesPerPort = 1;
    hrc = pController->COMGETTER(MaxDevicesPerPortCount)(&cMaxDevicesPerPort);
    AssertComRCReturn(hrc, hrc);
    AssertLogRelReturn(cMaxDevicesPerPort > 0, E_UNEXPECTED);

    ULONG cPorts = 0;
    hrc = pController->COMGETTER(PortCount)(&cPorts);
    AssertComRCReturn(hrc, hrc);

    /*
     * Get the attachment list and turn into an internal list for lookup speed.
     */
    com::SafeIfaceArray<IMediumAttachment> arrayOfMediumAttachments;
    hrc = rPtrSessionMachine->GetMediumAttachmentsOfController(Bstr(rStrControllerName).raw(),
                                                               ComSafeArrayAsOutParam(arrayOfMediumAttachments));
    AssertComRCReturn(hrc, hrc);

    std::vector<ControllerSlot> arrayOfUsedSlots;
    for (size_t i = 0; i < arrayOfMediumAttachments.size(); i++)
    {
        LONG iPort = -1;
        hrc = arrayOfMediumAttachments[i]->COMGETTER(Port)(&iPort);
        AssertComRCReturn(hrc, hrc);

        LONG iDevice = -1;
        hrc = arrayOfMediumAttachments[i]->COMGETTER(Device)(&iDevice);
        AssertComRCReturn(hrc, hrc);

        arrayOfUsedSlots.push_back(ControllerSlot(enmStorageBus, Utf8Str::Empty, iPort, iDevice, false /*fFree*/));
    }

    /*
     * Iterate thru all possible slots, adding those not found in arrayOfUsedSlots.
     */
    for (uint32_t iPort = 0; iPort < cPorts; iPort++)
        for (uint32_t iDevice = 0; iDevice < cMaxDevicesPerPort; iDevice++)
        {
            bool fFound = false;
            for (size_t i = 0; i < arrayOfUsedSlots.size(); i++)
                if (   arrayOfUsedSlots[i].uPort   == iPort
                    && arrayOfUsedSlots[i].uDevice == iDevice)
                {
                    fFound = true;
                    break;
                }
            if (!fFound)
            {
                rDvdSlots.push_back(ControllerSlot(enmStorageBus, rStrControllerName, iPort, iDevice, true /*fFree*/));
                if (rDvdSlots.size() >= cSlotsNeeded)
                    return S_OK;
            }
        }

    /*
     * Okay we still need more ports.  See if increasing the number of controller
     * ports would solve it.
     */
    ULONG cMaxPorts = 1;
    hrc = pController->COMGETTER(MaxPortCount)(&cMaxPorts);
    AssertComRCReturn(hrc, hrc);
    if (cMaxPorts <= cPorts)
        return S_OK;
    size_t cNewPortsNeeded = (cSlotsNeeded - rDvdSlots.size() + cMaxDevicesPerPort - 1) / cMaxDevicesPerPort;
    if (cPorts + cNewPortsNeeded > cMaxPorts)
        return S_OK;

    /*
     * Raise the port count and add the free slots we've just created.
     */
    hrc = pController->COMSETTER(PortCount)(cPorts + (ULONG)cNewPortsNeeded);
    AssertComRCReturn(hrc, hrc);
    for (uint32_t iPort = cPorts; iPort < cPorts + cNewPortsNeeded; iPort++)
        for (uint32_t iDevice = 0; iDevice < cMaxDevicesPerPort; iDevice++)
        {
            rDvdSlots.push_back(ControllerSlot(enmStorageBus, rStrControllerName, iPort, iDevice, true /*fFree*/));
            if (rDvdSlots.size() >= cSlotsNeeded)
                return S_OK;
        }

    /* We should not get here! */
    AssertLogRelFailedReturn(E_UNEXPECTED);
}

HRESULT Unattended::done()
{
    LogFlow(("Unattended::done\n"));
    if (mpInstaller)
    {
        LogRelFlow(("Unattended::done: Deleting installer object (%p)\n", mpInstaller));
        delete mpInstaller;
        mpInstaller = NULL;
    }
    return S_OK;
}

HRESULT Unattended::getIsoPath(com::Utf8Str &isoPath)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    isoPath = mStrIsoPath;
    return S_OK;
}

HRESULT Unattended::setIsoPath(const com::Utf8Str &isoPath)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mStrIsoPath = isoPath;
    return S_OK;
}

HRESULT Unattended::getUser(com::Utf8Str &user)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    user = mStrUser;
    return S_OK;
}


HRESULT Unattended::setUser(const com::Utf8Str &user)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mStrUser = user;
    return S_OK;
}

HRESULT Unattended::getPassword(com::Utf8Str &password)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    password = mStrPassword;
    return S_OK;
}

HRESULT Unattended::setPassword(const com::Utf8Str &password)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mStrPassword = password;
    return S_OK;
}

HRESULT Unattended::getFullUserName(com::Utf8Str &fullUserName)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    fullUserName = mStrFullUserName;
    return S_OK;
}

HRESULT Unattended::setFullUserName(const com::Utf8Str &fullUserName)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mStrFullUserName = fullUserName;
    return S_OK;
}

HRESULT Unattended::getProductKey(com::Utf8Str &productKey)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    productKey = mStrProductKey;
    return S_OK;
}

HRESULT Unattended::setProductKey(const com::Utf8Str &productKey)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mStrProductKey = productKey;
    return S_OK;
}

HRESULT Unattended::getAdditionsIsoPath(com::Utf8Str &additionsIsoPath)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    additionsIsoPath = mStrAdditionsIsoPath;
    return S_OK;
}

HRESULT Unattended::setAdditionsIsoPath(const com::Utf8Str &additionsIsoPath)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mStrAdditionsIsoPath = additionsIsoPath;
    return S_OK;
}

HRESULT Unattended::getInstallGuestAdditions(BOOL *installGuestAdditions)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    *installGuestAdditions = mfInstallGuestAdditions;
    return S_OK;
}

HRESULT Unattended::setInstallGuestAdditions(BOOL installGuestAdditions)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mfInstallGuestAdditions = installGuestAdditions != FALSE;
    return S_OK;
}

HRESULT Unattended::getValidationKitIsoPath(com::Utf8Str &aValidationKitIsoPath)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    aValidationKitIsoPath = mStrValidationKitIsoPath;
    return S_OK;
}

HRESULT Unattended::setValidationKitIsoPath(const com::Utf8Str &aValidationKitIsoPath)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mStrValidationKitIsoPath = aValidationKitIsoPath;
    return S_OK;
}

HRESULT Unattended::getInstallTestExecService(BOOL *aInstallTestExecService)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    *aInstallTestExecService = mfInstallTestExecService;
    return S_OK;
}

HRESULT Unattended::setInstallTestExecService(BOOL aInstallTestExecService)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mfInstallTestExecService = aInstallTestExecService != FALSE;
    return S_OK;
}

HRESULT Unattended::getTimeZone(com::Utf8Str &aTimeZone)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    aTimeZone = mStrTimeZone;
    return S_OK;
}

HRESULT Unattended::setTimeZone(const com::Utf8Str &aTimezone)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mStrTimeZone = aTimezone;
    return S_OK;
}

HRESULT Unattended::getLocale(com::Utf8Str &aLocale)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    aLocale = mStrLocale;
    return S_OK;
}

HRESULT Unattended::setLocale(const com::Utf8Str &aLocale)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    if (    aLocale.isEmpty() /* use default */
        || (   aLocale.length() == 5
            && RT_C_IS_LOWER(aLocale[0])
            && RT_C_IS_LOWER(aLocale[1])
            && aLocale[2] == '_'
            && RT_C_IS_UPPER(aLocale[3])
            && RT_C_IS_UPPER(aLocale[4])) )
    {
        mStrLocale = aLocale;
        return S_OK;
    }
    return setError(E_INVALIDARG, tr("Expected two lower cased letters, an underscore, and two upper cased letters"));
}

HRESULT Unattended::getCountry(com::Utf8Str &aCountry)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    aCountry = mStrCountry;
    return S_OK;
}

HRESULT Unattended::setCountry(const com::Utf8Str &aCountry)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    if (   aCountry.isEmpty()
        || (   aCountry.length() == 2
            && RT_C_IS_UPPER(aCountry[0])
            && RT_C_IS_UPPER(aCountry[1])) )
    {
        mStrCountry = aCountry;
        return S_OK;
    }
    return setError(E_INVALIDARG, tr("Expected two upper cased letters"));
}

HRESULT Unattended::getProxy(com::Utf8Str &aProxy)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    aProxy = ""; /// @todo turn schema map into string or something.
    return S_OK;
}

HRESULT Unattended::setProxy(const com::Utf8Str &aProxy)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    if (aProxy.isEmpty())
    {
        /* set default proxy */
    }
    else if (aProxy.equalsIgnoreCase("none"))
    {
        /* clear proxy config */
    }
    else
    {
        /* Parse and set proxy config into a schema map or something along those lines. */
        return E_NOTIMPL;
    }
    return S_OK;
}

HRESULT Unattended::getPackageSelectionAdjustments(com::Utf8Str &aPackageSelectionAdjustments)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    aPackageSelectionAdjustments = RTCString::join(mPackageSelectionAdjustments, ";");
    return S_OK;
}

HRESULT Unattended::setPackageSelectionAdjustments(const com::Utf8Str &aPackageSelectionAdjustments)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    if (aPackageSelectionAdjustments.isEmpty())
        mPackageSelectionAdjustments.clear();
    else
    {
        RTCList<RTCString, RTCString *> arrayStrSplit = aPackageSelectionAdjustments.split(";");
        for (size_t i = 0; i < arrayStrSplit.size(); i++)
        {
            if (arrayStrSplit[i].equals("minimal"))
            { /* okay */ }
            else
                return setError(E_INVALIDARG, tr("Unknown keyword: %s"), arrayStrSplit[i].c_str());
        }
        mPackageSelectionAdjustments = arrayStrSplit;
    }
    return S_OK;
}

HRESULT Unattended::getHostname(com::Utf8Str &aHostname)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    aHostname = mStrHostname;
    return S_OK;
}

HRESULT Unattended::setHostname(const com::Utf8Str &aHostname)
{
    /*
     * Validate input.
     */
    if (aHostname.length() > (aHostname.endsWith(".") ? 254U : 253U))
        return setErrorBoth(E_INVALIDARG, VERR_INVALID_NAME,
                            tr("Hostname '%s' is %zu bytes long, max is 253 (excluing trailing dot)"),
                            aHostname.c_str(), aHostname.length());
    size_t      cLabels  = 0;
    const char *pszSrc   = aHostname.c_str();
    for (;;)
    {
        size_t cchLabel = 1;
        char ch = *pszSrc++;
        if (RT_C_IS_ALNUM(ch))
        {
            cLabels++;
            while ((ch = *pszSrc++) != '.' && ch != '\0')
            {
                if (RT_C_IS_ALNUM(ch) || ch == '-')
                {
                    if (cchLabel < 63)
                        cchLabel++;
                    else
                        return setErrorBoth(E_INVALIDARG, VERR_INVALID_NAME,
                                            tr("Invalid hostname '%s' - label %u is too long, max is 63."),
                                            aHostname.c_str(), cLabels);
                }
                else
                    return setErrorBoth(E_INVALIDARG, VERR_INVALID_NAME,
                                        tr("Invalid hostname '%s' - illegal char '%c' at position %zu"),
                                        aHostname.c_str(), ch, pszSrc - aHostname.c_str() - 1);
            }
            if (cLabels == 1 && cchLabel < 2)
                return setErrorBoth(E_INVALIDARG, VERR_INVALID_NAME,
                                    tr("Invalid hostname '%s' - the name part must be at least two characters long"),
                                    aHostname.c_str());
            if (ch == '\0')
                break;
        }
        else if (ch != '\0')
            return setErrorBoth(E_INVALIDARG, VERR_INVALID_NAME,
                                tr("Invalid hostname '%s' - illegal lead char '%c' at position %zu"),
                                aHostname.c_str(), ch, pszSrc - aHostname.c_str() - 1);
        else
            return setErrorBoth(E_INVALIDARG, VERR_INVALID_NAME,
                                tr("Invalid hostname '%s' - trailing dot not permitted"), aHostname.c_str());
    }
    if (cLabels < 2)
        return setErrorBoth(E_INVALIDARG, VERR_INVALID_NAME,
                            tr("Incomplete hostname '%s' - must include both a name and a domain"), aHostname.c_str());

    /*
     * Make the change.
     */
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mStrHostname = aHostname;
    return S_OK;
}

HRESULT Unattended::getAuxiliaryBasePath(com::Utf8Str &aAuxiliaryBasePath)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    aAuxiliaryBasePath = mStrAuxiliaryBasePath;
    return S_OK;
}

HRESULT Unattended::setAuxiliaryBasePath(const com::Utf8Str &aAuxiliaryBasePath)
{
    if (aAuxiliaryBasePath.isEmpty())
        return setError(E_INVALIDARG, "Empty base path is not allowed");
    if (!RTPathStartsWithRoot(aAuxiliaryBasePath.c_str()))
        return setError(E_INVALIDARG, "Base path must be absolute");

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mStrAuxiliaryBasePath = aAuxiliaryBasePath;
    mfIsDefaultAuxiliaryBasePath = mStrAuxiliaryBasePath.isEmpty();
    return S_OK;
}

HRESULT Unattended::getImageIndex(ULONG *index)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    *index = midxImage;
    return S_OK;
}

HRESULT Unattended::setImageIndex(ULONG index)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    midxImage = index;
    return S_OK;
}

HRESULT Unattended::getMachine(ComPtr<IMachine> &aMachine)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    return mMachine.queryInterfaceTo(aMachine.asOutParam());
}

HRESULT Unattended::setMachine(const ComPtr<IMachine> &aMachine)
{
    /*
     * Lookup the VM so we can safely get the Machine instance.
     * (Don't want to test how reliable XPCOM and COM are with finding
     * the local object instance when a client passes a stub back.)
     */
    Bstr bstrUuidMachine;
    HRESULT hrc = aMachine->COMGETTER(Id)(bstrUuidMachine.asOutParam());
    if (SUCCEEDED(hrc))
    {
        Guid UuidMachine(bstrUuidMachine);
        ComObjPtr<Machine> ptrMachine;
        hrc = mParent->i_findMachine(UuidMachine, false /*fPermitInaccessible*/, true /*aSetError*/, &ptrMachine);
        if (SUCCEEDED(hrc))
        {
            AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
            AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER,
                                                           tr("Cannot change after prepare() has been called")));
            mMachine     = ptrMachine;
            mMachineUuid = UuidMachine;
            if (mfIsDefaultAuxiliaryBasePath)
                mStrAuxiliaryBasePath.setNull();
            hrc = S_OK;
        }
    }
    return hrc;
}

HRESULT Unattended::getScriptTemplatePath(com::Utf8Str &aScriptTemplatePath)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    if (   mStrScriptTemplatePath.isNotEmpty()
        || mpInstaller == NULL)
        aScriptTemplatePath = mStrScriptTemplatePath;
    else
        aScriptTemplatePath = mpInstaller->getTemplateFilePath();
    return S_OK;
}

HRESULT Unattended::setScriptTemplatePath(const com::Utf8Str &aScriptTemplatePath)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mStrScriptTemplatePath = aScriptTemplatePath;
    return S_OK;
}

HRESULT Unattended::getPostInstallScriptTemplatePath(com::Utf8Str &aPostInstallScriptTemplatePath)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    if (   mStrPostInstallScriptTemplatePath.isNotEmpty()
        || mpInstaller == NULL)
        aPostInstallScriptTemplatePath = mStrPostInstallScriptTemplatePath;
    else
        aPostInstallScriptTemplatePath = mpInstaller->getPostTemplateFilePath();
    return S_OK;
}

HRESULT Unattended::setPostInstallScriptTemplatePath(const com::Utf8Str &aPostInstallScriptTemplatePath)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mStrPostInstallScriptTemplatePath = aPostInstallScriptTemplatePath;
    return S_OK;
}

HRESULT Unattended::getPostInstallCommand(com::Utf8Str &aPostInstallCommand)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    aPostInstallCommand = mStrPostInstallCommand;
    return S_OK;
}

HRESULT Unattended::setPostInstallCommand(const com::Utf8Str &aPostInstallCommand)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mStrPostInstallCommand = aPostInstallCommand;
    return S_OK;
}

HRESULT Unattended::getExtraInstallKernelParameters(com::Utf8Str &aExtraInstallKernelParameters)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    if (   mStrExtraInstallKernelParameters.isNotEmpty()
        || mpInstaller == NULL)
        aExtraInstallKernelParameters = mStrExtraInstallKernelParameters;
    else
        aExtraInstallKernelParameters = mpInstaller->getDefaultExtraInstallKernelParameters();
    return S_OK;
}

HRESULT Unattended::setExtraInstallKernelParameters(const com::Utf8Str &aExtraInstallKernelParameters)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    AssertReturn(mpInstaller == NULL, setErrorBoth(E_FAIL, VERR_WRONG_ORDER, tr("Cannot change after prepare() has been called")));
    mStrExtraInstallKernelParameters = aExtraInstallKernelParameters;
    return S_OK;
}

HRESULT Unattended::getDetectedOSTypeId(com::Utf8Str &aDetectedOSTypeId)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    aDetectedOSTypeId = mStrDetectedOSTypeId;
    return S_OK;
}

HRESULT Unattended::getDetectedOSVersion(com::Utf8Str &aDetectedOSVersion)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    aDetectedOSVersion = mStrDetectedOSVersion;
    return S_OK;
}

HRESULT Unattended::getDetectedOSFlavor(com::Utf8Str &aDetectedOSFlavor)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    aDetectedOSFlavor = mStrDetectedOSFlavor;
    return S_OK;
}

HRESULT Unattended::getDetectedOSHints(com::Utf8Str &aDetectedOSHints)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    aDetectedOSHints = mStrDetectedOSHints;
    return S_OK;
}

/*
 * Getters that the installer and script classes can use.
 */
Utf8Str const &Unattended::i_getIsoPath() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrIsoPath;
}

Utf8Str const &Unattended::i_getUser() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrUser;
}

Utf8Str const &Unattended::i_getPassword() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrPassword;
}

Utf8Str const &Unattended::i_getFullUserName() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrFullUserName.isNotEmpty() ? mStrFullUserName : mStrUser;
}

Utf8Str const &Unattended::i_getProductKey() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrProductKey;
}

Utf8Str const &Unattended::i_getAdditionsIsoPath() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrAdditionsIsoPath;
}

bool           Unattended::i_getInstallGuestAdditions() const
{
    Assert(isReadLockedOnCurrentThread());
    return mfInstallGuestAdditions;
}

Utf8Str const &Unattended::i_getValidationKitIsoPath() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrValidationKitIsoPath;
}

bool           Unattended::i_getInstallTestExecService() const
{
    Assert(isReadLockedOnCurrentThread());
    return mfInstallTestExecService;
}

Utf8Str const &Unattended::i_getTimeZone() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrTimeZone;
}

PCRTTIMEZONEINFO Unattended::i_getTimeZoneInfo() const
{
    Assert(isReadLockedOnCurrentThread());
    return mpTimeZoneInfo;
}

Utf8Str const &Unattended::i_getLocale() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrLocale;
}

Utf8Str const &Unattended::i_getCountry() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrCountry;
}

bool Unattended::i_isMinimalInstallation() const
{
    size_t i = mPackageSelectionAdjustments.size();
    while (i-- > 0)
        if (mPackageSelectionAdjustments[i].equals("minimal"))
            return true;
    return false;
}

Utf8Str const &Unattended::i_getHostname() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrHostname;
}

Utf8Str const &Unattended::i_getAuxiliaryBasePath() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrAuxiliaryBasePath;
}

ULONG Unattended::i_getImageIndex() const
{
    Assert(isReadLockedOnCurrentThread());
    return midxImage;
}

Utf8Str const &Unattended::i_getScriptTemplatePath() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrScriptTemplatePath;
}

Utf8Str const &Unattended::i_getPostInstallScriptTemplatePath() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrPostInstallScriptTemplatePath;
}

Utf8Str const &Unattended::i_getPostInstallCommand() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrPostInstallCommand;
}

Utf8Str const &Unattended::i_getExtraInstallKernelParameters() const
{
    Assert(isReadLockedOnCurrentThread());
    return mStrExtraInstallKernelParameters;
}

bool Unattended::i_isRtcUsingUtc() const
{
    Assert(isReadLockedOnCurrentThread());
    return mfRtcUseUtc;
}

bool Unattended::i_isGuestOs64Bit() const
{
    Assert(isReadLockedOnCurrentThread());
    return mfGuestOs64Bit;
}

VBOXOSTYPE Unattended::i_getGuestOsType() const
{
    Assert(isReadLockedOnCurrentThread());
    return meGuestOsType;
}

HRESULT Unattended::i_attachImage(UnattendedInstallationDisk const *pImage, ComPtr<IMachine> const &rPtrSessionMachine,
                                  AutoMultiWriteLock2 &rLock)
{
    /*
     * Attach the disk image
     * HACK ALERT! Temporarily release the Unattended lock.
     */
    rLock.release();

    ComPtr<IMedium> ptrMedium;
    HRESULT rc = mParent->OpenMedium(Bstr(pImage->strImagePath).raw(),
                                     pImage->enmDeviceType,
                                     pImage->enmAccessType,
                                     true,
                                     ptrMedium.asOutParam());
    LogRelFlowFunc(("VirtualBox::openMedium -> %Rhrc\n", rc));
    if (SUCCEEDED(rc))
    {
        if (pImage->fMountOnly)
        {
            // mount the opened disk image
            rc = rPtrSessionMachine->MountMedium(Bstr(pImage->strControllerName).raw(), pImage->uPort,
                                                 pImage->uDevice, ptrMedium, TRUE /*fForce*/);
            LogRelFlowFunc(("Machine::MountMedium -> %Rhrc\n", rc));
        }
        else
        {
            //attach the opened disk image to the controller
            rc = rPtrSessionMachine->AttachDevice(Bstr(pImage->strControllerName).raw(), pImage->uPort,
                                                  pImage->uDevice, pImage->enmDeviceType, ptrMedium);
            LogRelFlowFunc(("Machine::AttachDevice -> %Rhrc\n", rc));
        }
    }

    rLock.acquire();
    return rc;
}

bool Unattended::i_isGuestOSArchX64(Utf8Str const &rStrGuestOsTypeId)
{
    ComPtr<IGuestOSType> pGuestOSType;
    HRESULT hrc = mParent->GetGuestOSType(Bstr(rStrGuestOsTypeId).raw(), pGuestOSType.asOutParam());
    if (SUCCEEDED(hrc))
    {
        BOOL fIs64Bit = FALSE;
        hrc = pGuestOSType->COMGETTER(Is64Bit)(&fIs64Bit);
        if (SUCCEEDED(hrc))
            return fIs64Bit != FALSE;
    }
    return false;
}

