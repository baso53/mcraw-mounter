//
//  MyFSItem.swift
//  FSKitExp12
//
//  Created by Khaos Tian on 3/30/25.
//

import Foundation
import FSKit
import MotionCamModule

final class MyFSItem: FSItem {

    let name: FSFileName

    let timestamp: MotionCamModule.motioncam.Timestamp
    
    let attributes = FSItem.Attributes()
    let metadata: FrameMetadata
    
    init(name: FSFileName, timestamp: MotionCamModule.motioncam.Timestamp, metadata: FrameMetadata, size: UInt64) {
        self.name = name
        self.timestamp = timestamp
        self.metadata = metadata
        attributes.fileID = FSItem.Identifier(rawValue: UInt64(timestamp)) ?? .invalid
        attributes.size = size
        attributes.allocSize = 0
        attributes.flags = 0
        attributes.mode = UInt32(S_IFDIR | 0b111_000_000)

        attributes.uid  = getuid()
        attributes.gid  = getgid()

        var timespec = timespec()
        timespec_get(&timespec, TIME_UTC)
        
        attributes.addedTime = timespec
        attributes.birthTime = timespec
        attributes.changeTime = timespec
        attributes.modifyTime = timespec
        attributes.accessTime = timespec
        attributes.type = .file
        
        attributes.parentID = .rootDirectory
        attributes.linkCount = 0
    }
}
