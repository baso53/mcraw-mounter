//
//  FSKitExp12Extension.swift
//  FSKitExp12Extension
//
//  Created by Khaos Tian on 3/30/25.
//

import Foundation
import FSKit

@main
struct FSKitExp12Extension : UnaryFileSystemExtension {
    
    var fileSystem : FSUnaryFileSystem & FSUnaryFileSystemOperations {
        MyFS()
    }
}
