//
//  MyFS.swift
//  FSKitExp12
//
//  Created by Khaos Tian on 3/30/25.
//

import Foundation
import FSKit
import os

final class MyFS: FSUnaryFileSystem, FSUnaryFileSystemOperations {
    
    private let logger = Logger(subsystem: "FSKitExp12", category: "MyFS")
    
    func probeResource(
        resource: FSResource,
        replyHandler: @escaping (FSProbeResult?, (any Error)?) -> Void
    ) {
        replyHandler(
            FSProbeResult.usable(
                name: "Test5",
                containerID: FSContainerIdentifier(uuid: UUID())
            ),
            nil
        )
    }
    
    func loadResource(
        resource: FSResource,
        options: FSTaskOptions,
        replyHandler: @escaping (FSVolume?, (any Error)?) -> Void
    ) {
        containerStatus = .ready
        let volume = MyFSVolume(resource: resource)
        replyHandler(
            volume,
            nil
        )
    }
    
    func unloadResource(
        resource: FSResource,
        options: FSTaskOptions,
        replyHandler reply: @escaping ((any Error)?) -> Void
    ) {
        logger.debug("unloadResource: \(resource, privacy: .public)")
        reply(nil)
    }
    
    func didFinishLoading() {
        logger.debug("didFinishLoading")
    }
}
