//
//  ContainerMetadata.swift
//  FSKitExpExtension
//
//  Created by sebastijan on 01.09.2025..
//


public struct ContainerMetadata: Codable {
    public let blackLevel: [UInt16]
    public let whiteLevel: Double
    public let sensorArrangement: String
    public let colorMatrix1: [Float]
    public let colorMatrix2: [Float]
    public let forwardMatrix1: [Float]
    public let forwardMatrix2: [Float]
    
    private enum CodingKeys: String, CodingKey {
        case blackLevel
        case whiteLevel
        // JSON key has a typo "sensorArrangment" so we remap it:
        case sensorArrangement = "sensorArrangment"
        case colorMatrix1
        case colorMatrix2
        case forwardMatrix1
        case forwardMatrix2
    }
}
