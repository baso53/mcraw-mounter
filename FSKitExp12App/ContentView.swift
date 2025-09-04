import Foundation
import SwiftUI
import UniformTypeIdentifiers

func attachRawDiskImage(at path: String,
                        completion: @escaping (Result<String, Error>) -> Void)
{
    let process = Process()
    process.executableURL = URL(fileURLWithPath: "/usr/bin/hdiutil")
    process.arguments = [
        "attach",
        "-imagekey", "diskimage-class=CRawDiskImage",
        "-nomount",
        path
    ]

    let outputPipe = Pipe()
    process.standardOutput = outputPipe
    process.standardError = outputPipe

    do {
        try process.run()
    }
    catch {
        completion(.failure(error))
        return
    }

    process.terminationHandler = { proc in
        let data = outputPipe.fileHandleForReading.readDataToEndOfFile()
        let output = String(decoding: data, as: UTF8.self)
        if proc.terminationStatus == 0 {
            completion(.success(output))
        } else {
            let err = NSError(domain: "hdiutil", code: Int(proc.terminationStatus), userInfo: [
                NSLocalizedDescriptionKey: output
            ])
            completion(.failure(err))
        }
    }
}

struct ContentView: View {
    @State private var resultText1 = "Not run yet"
    @State private var resultText2 = "Not run yet"
    @State private var bookmarkData: Data?
    @State private var showingImporter = false

    var body: some View {
        VStack(spacing: 20) {
            Button("Attach Raw Image") {
                let imgPath = "/Users/sebastijan/Desktop/dummy"
                attachRawDiskImage(at: imgPath) { result in
                    DispatchQueue.main.async {
                        switch result {
                        case .success(let output):
                            resultText1 = "Success:\n" + output
                        case .failure(let error):
                            resultText1 = "Error:\n" + error.localizedDescription
                        }
                    }
                }
            }

            Text(resultText1)
                .padding()

            Button("Select Folder…") {
                showingImporter = true
            }
        }
    }
}
