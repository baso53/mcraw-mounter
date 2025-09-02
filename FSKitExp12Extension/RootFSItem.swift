import Foundation
import FSKit
import MotionCamModule

final class RootFSItem: FSItem {
    
    let name: FSFileName

    var attributes = FSItem.Attributes()
   
    private(set) var children: [FSFileName: MyFSItem] = [:]
    
    var containerMetadata: ContainerMetadata
    
    var decoder: MotionCamModule.motioncam.Decoder

    let maxCacheFrames = 5
    var frameCache: [MotionCamModule.motioncam.Timestamp: Data] = [:]
    var frameCacheOrder: [MotionCamModule.motioncam.Timestamp] = []
    
    var cacheLock = NSLock()
    
    init(name: FSFileName, decoder: consuming MotionCamModule.motioncam.Decoder) {
        self.name = name
        self.decoder = decoder
        
        let containerMetadataJson = String(self.decoder.getContainerMetadata())
        
        do {
            self.containerMetadata = try JSONDecoder().decode(ContainerMetadata.self, from: containerMetadataJson.data(using: .utf8)!)
        } catch {
            print("error decoding")
            exit(EXIT_FAILURE)
        }
        
        var timespec = timespec()
        timespec_get(&timespec, TIME_UTC)
        
        attributes.addedTime = timespec
        attributes.birthTime = timespec
        attributes.changeTime = timespec
        attributes.modifyTime = timespec
        attributes.accessTime = timespec

        attributes.parentID = .parentOfRoot
        attributes.fileID = .rootDirectory
        attributes.uid  = getuid()
        attributes.gid  = getgid()
        attributes.linkCount = 0
        attributes.type = .directory
        attributes.mode = UInt32(S_IFDIR | 0b111_000_000)
        attributes.allocSize = 1
        attributes.size = 1
        attributes.flags = 0
    }
    
    func addItem(_ item: MyFSItem) {
        children[item.name] = item
        item.attributes.parentID = attributes.fileID
    }
}
