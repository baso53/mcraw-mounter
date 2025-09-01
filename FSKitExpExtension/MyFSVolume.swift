import Foundation
import FSKit
import os
import MotionCamModule
import TinyDngModule

final class MyFSVolume: FSVolume {
    
    private let resource: FSResource
    
    private let logger = Logger(subsystem: "FSKitExp", category: "MyFSVolume")
    
    private let root: RootFSItem

    init(resource: FSResource) {
        self.resource = resource
        
        do {
            root = RootFSItem(name: FSFileName(string: "/"), decoder: MotionCamModule.motioncam.Decoder("/Users/sebastijan/007-VIDEO_24mm-240328_141729.0.mcraw"))
        
            let frameTimestamps = root.decoder.getFrames()
            
            let firstFrameMetadataJson = String(root.decoder.loadFrameMetadata(frameTimestamps.first!))
            let firstFrameMetadata = try JSONDecoder().decode(FrameMetadata.self, from: firstFrameMetadataJson.data(using: .utf8)!)

            root.frameFileSize = UInt64(MyFSVolume.getData(
                timestamp: frameTimestamps.first!,
                frameMetadata: firstFrameMetadata,
                containerMetadata: root.containerMetadata,
                rootItem: root
            ).count)
            
            // Create a child MyFSItem for each frame timestamp
            for (index, timestamp) in frameTimestamps.prefix(100).enumerated() {
                let frameMetadataJson = String(root.decoder.loadFrameMetadata(timestamp))
                let frameMetadata = try JSONDecoder().decode(FrameMetadata.self, from: frameMetadataJson.data(using: .utf8)!)

                // now `data` holds the exact bytes from your CChar buffer
                let nameString = "frame_\(index).dng"
                let fileName = FSFileName(string: nameString)
                
                let frameItem = MyFSItem(name: fileName, timestamp: timestamp, metadata: frameMetadata, size: root.frameFileSize)
                root.addItem(frameItem)
            }
        
        super.init(
            volumeID: FSVolume.Identifier(uuid: UUID()),
            volumeName: FSFileName(string: "Test5")
        )
        } catch {
            print("Decoding failed:", error)
            exit(EXIT_FAILURE)
        }
    }
    
    static func getData(
        timestamp: MotionCamModule.motioncam.Timestamp,
        frameMetadata: FrameMetadata,
        containerMetadata: ContainerMetadata,
        rootItem: RootFSItem
    ) -> Data {
        // 1) Check cache
        if let cached = rootItem.frameCache[timestamp] {
          return cached
        }

        var outData = motioncam.FrameOutData()
        
        rootItem.decoder.loadFrame(timestamp, &outData, frameMetadata.width, frameMetadata.height, frameMetadata.compressionType)
        
        var dng = TinyDngModule.tinydngwriter.DNGImage()
        dng.SetBigEndian(false);
        dng.SetDNGVersion(1, 4, 0, 0);
        dng.SetDNGBackwardVersion(1, 1, 0, 0);
        dng.SetImageData(
            outData,
            outData.size());
        dng.SetImageWidth(UInt32(frameMetadata.width));
        dng.SetImageLength(UInt32(frameMetadata.height));
        dng.SetPlanarConfig(UInt16(tinydngwriter.PLANARCONFIG_CONTIG));
        dng.SetPhotometric(UInt16(tinydngwriter.PHOTOMETRIC_CFA));
        dng.SetRowsPerStrip(UInt32(frameMetadata.height));
        dng.SetSamplesPerPixel(1);
        dng.SetCFARepeatPatternDim(2, 2);
        
        dng.SetBlackLevelRepeatDim(2, 2);
        dng.SetBlackLevel(4, rootItem.containerMetadata.blackLevel);
        dng.SetWhiteLevel(Int16(rootItem.containerMetadata.whiteLevel));
        dng.SetCompression(UInt16(tinydngwriter.COMPRESSION_NONE));
        
        var cfa: MotionCamModule.motioncam.CFA
        
        if(rootItem.containerMetadata.sensorArrangement == "rggb") {
            cfa = MotionCamModule.motioncam.CFA(arrayLiteral: 0, 1, 1, 2);
        }
        else if(rootItem.containerMetadata.sensorArrangement == "bggr") {
            cfa = MotionCamModule.motioncam.CFA(arrayLiteral: 2, 1, 1, 0);
        }
        else if(rootItem.containerMetadata.sensorArrangement == "grbg") {
            cfa = MotionCamModule.motioncam.CFA(arrayLiteral: 1, 0, 2, 1);
        }
        else if(rootItem.containerMetadata.sensorArrangement == "gbrg") {
            cfa = MotionCamModule.motioncam.CFA(arrayLiteral: 1, 2, 0, 1);
        } else {
            cfa = MotionCamModule.motioncam.CFA(arrayLiteral: 1, 2, 0, 1);
        }
        
        dng.SetCFAPattern(4, &cfa);
        
        // Rectangular
        dng.SetCFALayout(1);
        
        var bps = MotionCamModule.motioncam.BPS(16)
        dng.SetBitsPerSample(1, &bps);
        
        dng.SetColorMatrix1(3, rootItem.containerMetadata.colorMatrix1);
        dng.SetColorMatrix2(3, rootItem.containerMetadata.colorMatrix2);
        
        dng.SetForwardMatrix1(3, rootItem.containerMetadata.forwardMatrix1);
        dng.SetForwardMatrix2(3, rootItem.containerMetadata.forwardMatrix2);
        
        dng.SetAsShotNeutral(3, frameMetadata.asShotNeutral);
        
        dng.SetCalibrationIlluminant1(21);
        dng.SetCalibrationIlluminant2(17);
        
        dng.SetUniqueCameraModel("MotionCam");
        dng.SetSubfileType();
        
        var activeArea = MotionCamModule.motioncam.ActiveArea( 0, 0, UInt32(frameMetadata.height), UInt32(frameMetadata.width));
        dng.SetActiveArea(&activeArea.0);

        var err = std.string()
        var count = motioncam.Count()
        
        var writer = tinydngwriter.DNGWriter(false)
        writer.AddImage(&dng)
        
        let str = writer.WriteToFile(&err, &count)

        let data = Data(bytesNoCopy: UnsafeMutableRawPointer(mutating: str!), count: Int(count), deallocator: .free)

        // 3) Insert into cache, popping oldest if needed
        if rootItem.frameCache.count >= rootItem.maxCacheFrames {
            let oldestKey = rootItem.frameCacheOrder.removeFirst()
            rootItem.frameCache[oldestKey] = nil
        }
        rootItem.frameCache[timestamp] = data
        rootItem.frameCacheOrder.append(timestamp)

        return data
    }
}

extension MyFSVolume: FSVolume.PathConfOperations {
    
    var maximumLinkCount: Int {
        return -1
    }
    
    var maximumNameLength: Int {
        return -1
    }
    
    var restrictsOwnershipChanges: Bool {
        return false
    }
    
    var truncatesLongNames: Bool {
        return false
    }
    
    var maximumXattrSize: Int {
        return Int.max
    }
    
    var maximumFileSize: UInt64 {
        return UInt64.max
    }
}

extension MyFSVolume: FSVolume.Operations {
    
    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
//        logger.debug("supportedVolumeCapabilities")
        
        let capabilities = FSVolume.SupportedCapabilities()
        capabilities.supportsHardLinks = false
        capabilities.supportsSymbolicLinks = false
        capabilities.supportsPersistentObjectIDs = true
        capabilities.doesNotSupportVolumeSizes = true
        capabilities.supportsHiddenFiles = false
        capabilities.supports64BitObjectIDs = true
        capabilities.caseFormat = .insensitiveCasePreserving
        return capabilities
    }
    
    var volumeStatistics: FSStatFSResult {
//        logger.debug("volumeStatistics")
        
        let result = FSStatFSResult(fileSystemTypeName: "MyFS")
        
        result.blockSize = 1024000
        result.ioSize = 1024000
        result.totalBlocks = 1024000
        result.availableBlocks = 1024000
        result.freeBlocks = 1024000
        result.totalFiles = 1024000
        result.freeFiles = 1024000
        
        return result
    }
    
    
    func activate(options: FSTaskOptions) async throws -> FSItem {
//        logger.debug("activate")
        return root
    }
    
    func deactivate(options: FSDeactivateOptions = []) async throws {
//        logger.debug("deactivate")
    }
    
    func mount(options: FSTaskOptions) async throws {
//        logger.debug("mount")
    }
    
    func unmount() async {
//        logger.debug("unmount")
    }
    
    func synchronize(flags: FSSyncFlags) async throws {
//        logger.debug("synchronize")
    }
    
    func attributes(
        _ desiredAttributes: FSItem.GetAttributesRequest,
        of item: FSItem
    ) async throws -> FSItem.Attributes {
        if let item = item as? MyFSItem {
//            logger.debug("getItemAttributes for MyFSItem: \(item.name), \(desiredAttributes)")
            return item.attributes
        } else if let item = item as? RootFSItem {
//            logger.debug("getItemAttributes for RootFSItem: \(item.name), \(desiredAttributes)")
            return item.attributes
        } else {
//            logger.debug("getItemAttributes error: \(item), \(desiredAttributes)")
            throw fs_errorForPOSIXError(POSIXError.EIO.rawValue)
        }
    }
    
    func setAttributes(
        _ newAttributes: FSItem.SetAttributesRequest,
        on item: FSItem
    ) async throws -> FSItem.Attributes {
        throw fs_errorForPOSIXError(POSIXError.EIO.rawValue)
    }
    
    func lookupItem(
        named name: FSFileName,
        inDirectory directory: FSItem
    ) async throws -> (FSItem, FSFileName) {
//        logger.debug("lookupName: \(String(describing: name.string)), \(directory)")
        
        guard let directory = directory as? RootFSItem else {
            throw fs_errorForPOSIXError(POSIXError.ENOENT.rawValue)
        }
        
        for (key, child) in directory.children {
            if key.string == name.string {
                return (child, key)
            }
        }
        
        throw fs_errorForPOSIXError(POSIXError.ENOENT.rawValue)
    }
    
    func reclaimItem(_ item: FSItem) async throws {
        throw fs_errorForPOSIXError(POSIXError.EIO.rawValue)
    }
    
    func readSymbolicLink(
        _ item: FSItem
    ) async throws -> FSFileName {
        throw fs_errorForPOSIXError(POSIXError.EIO.rawValue)
    }
    
    func createItem(
        named name: FSFileName,
        type: FSItem.ItemType,
        inDirectory directory: FSItem,
        attributes newAttributes: FSItem.SetAttributesRequest
    ) async throws -> (FSItem, FSFileName) {
        throw fs_errorForPOSIXError(POSIXError.EIO.rawValue)
    }
    
    func createSymbolicLink(
        named name: FSFileName,
        inDirectory directory: FSItem,
        attributes newAttributes: FSItem.SetAttributesRequest,
        linkContents contents: FSFileName
    ) async throws -> (FSItem, FSFileName) {
        throw fs_errorForPOSIXError(POSIXError.EIO.rawValue)
    }
    
    func createLink(
        to item: FSItem,
        named name: FSFileName,
        inDirectory directory: FSItem
    ) async throws -> FSFileName {
        throw fs_errorForPOSIXError(POSIXError.EIO.rawValue)
    }
    
    func removeItem(
        _ item: FSItem,
        named name: FSFileName,
        fromDirectory directory: FSItem
    ) async throws {
        throw fs_errorForPOSIXError(POSIXError.EIO.rawValue)
    }
    
    func renameItem(
        _ item: FSItem,
        inDirectory sourceDirectory: FSItem,
        named sourceName: FSFileName,
        to destinationName: FSFileName,
        inDirectory destinationDirectory: FSItem,
        overItem: FSItem?
    ) async throws -> FSFileName {
        throw fs_errorForPOSIXError(POSIXError.EIO.rawValue)
    }
    
    func enumerateDirectory(
        _ directory: FSItem,
        startingAt cookie: FSDirectoryCookie,
        verifier: FSDirectoryVerifier,
        attributes: FSItem.GetAttributesRequest?,
        packer: FSDirectoryEntryPacker
    ) async throws -> FSDirectoryVerifier {
//        logger.debug("enumerateDirectory: \(directory)")
        
        guard let directory = directory as? RootFSItem else {
            throw fs_errorForPOSIXError(POSIXError.ENOENT.rawValue)
        }
        
//        logger.debug("- enumerateDirectory - \(directory.name)")
        
        for (idx, item) in directory.children.values.enumerated() {
            let isLast = (idx == directory.children.count - 1)
            
            let v = packer.packEntry(
                name: item.name,
                itemType: item.attributes.type,
                itemID: item.attributes.fileID,
                nextCookie: FSDirectoryCookie(UInt64(idx)),
                attributes: attributes != nil ? item.attributes : nil
            )
            
//            logger.debug("-- V: \(v) - \(item.name)")
        }
        
        return FSDirectoryVerifier(0)
    }
}

extension MyFSVolume: FSVolume.ReadWriteOperations {
    
    func read(
        from item: FSItem,
        at offset: off_t,
        length: Int,
        into buffer: FSMutableFileDataBuffer
    ) async throws -> Int {
//        logger.debug("read: \(item)")
        
        var bytesRead = 0
        
        if let item = item as? MyFSItem
        {
            let dng = MyFSVolume.getData(
              timestamp: item.timestamp,
              frameMetadata: item.metadata,
              containerMetadata: root.containerMetadata,
              rootItem: root
            )
            
            // Make sure offset is in range
            let totalSize = item.attributes.size
            guard offset < totalSize else {
                // nothing to read beyond EOF
                return 0
            }
            
            // Compute how many bytes we can actually read
            let maxRead = min(length, Int(totalSize) - Int(offset))
            
            // Copy bytes from `data` into your buffer
            bytesRead = dng.withUnsafeBytes { (src: UnsafeRawBufferPointer) in
                buffer.withUnsafeMutableBytes { (dst: UnsafeMutableRawBufferPointer) in
                    let asdasd = UnsafeRawPointer(src.baseAddress!)
                    let srcPtr = asdasd.advanced(by: Int(offset))
                    let dstPtr = dst.baseAddress!
                    memcpy(dstPtr, srcPtr, maxRead)
                    return maxRead
                }
            }
        }
        
        return bytesRead
    }
    
    func write(contents: Data, to item: FSItem, at offset: off_t) async throws -> Int {
        throw fs_errorForPOSIXError(POSIXError.EIO.rawValue)
    }
}
