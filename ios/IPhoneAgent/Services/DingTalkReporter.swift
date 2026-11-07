import Foundation
import CryptoKit

/// What a finished run looks like to a reporter. Built from `Run` by the caller so the reporter
/// stays independent of the trace-folder layout.
struct RunReport {
    let task: String
    let date: Date
    let steps: Int
    let duration: Double?
    let modelCalls: Int
    let tokens: Int
    /// false when the run was stopped or failed before it finished.
    let ok: Bool
    /// Screenshots to attach, already capped and ordered by the caller.
    let frames: [URL]
}

/// Sends a run report somewhere (DingTalk today, anything else tomorrow).
/// Nothing is ever sent unless an implementation is set AND the user agreed.
protocol RunReporter {
    func report(_ r: RunReport) async throws
}

enum ReportUpload {
    static let consentKey = "runReportConsent"
    static let webhookKey = "dingTalkWebhook"
    static let secretKey = "dingTalkSecret"

    /// Off until the user agrees, same shape as `LogUpload.consented`.
    static var consented: Bool {
        get { UserDefaults.standard.bool(forKey: consentKey) }
        set { UserDefaults.standard.set(newValue, forKey: consentKey) }
    }

    /// The robot webhook. Kept in the Keychain, not UserDefaults: it carries the access token,
    /// which is as good as a password.
    static var webhook: String {
        get { Keychain.get(webhookKey) ?? "" }
        set { Keychain.set(newValue, for: webhookKey) }
    }

    /// The robot's signing secret, if "加签" is enabled on it. Empty = keyword/IP mode.
    static var secret: String {
        get { Keychain.get(secretKey) ?? "" }
        set { Keychain.set(newValue, for: secretKey) }
    }

    static var configured: Bool { !webhook.isEmpty }

    /// Set once the webhook is filled in; nil = nothing is ever sent.
    @MainActor static var reporter: RunReporter?

    /// Upload only with consent, a webhook and a reporter. Returns whether it was sent.
    @MainActor static func reportIfAllowed(_ r: RunReport) async -> Bool {
        guard consented, configured, let reporter else { return false }
        do {
            try await reporter.report(r)
            DebugLog.write("run report sent")
            return true
        } catch {
            DebugLog.write("run report failed: \(error.localizedDescription)")
            return false
        }
    }
}

/// DingTalk custom robot (自定义机器人).
///
/// A robot is configured with exactly one of three security modes, and the mode decides the URL:
///   - 加签 (signing):   append `&timestamp=&sign=` — HMAC-SHA256(timestamp + "\n" + secret)
///   - 关键词 (keyword): the message must contain the configured keyword, so it goes in the title
///   - IP 白名单:        nothing to add, the caller's IP must be listed
/// Only the first needs code; the other two work with a plain webhook.
///
/// Images need two calls: upload to get a `media_id`, then send it. That is why frames are capped.
struct DingTalkReporter: RunReporter {
    /// DingTalk rejects a robot message larger than 20 KB, and rate-limits to 20/min.
    static let maxFrames = 3

    private let session: URLSession
    private let now: () -> Date

    init(session: URLSession = .shared, now: @escaping () -> Date = { Date() }) {
        self.session = session
        self.now = now
    }

    private enum E: LocalizedError {
        case badWebhook
        case http(Int, String)
        case noMediaId

        var errorDescription: String? {
            switch self {
            case .badWebhook: return String(localized: "DingTalk webhook is not a valid URL")
            case .http(let c, let body): return String(localized: "DingTalk returned HTTP \(c): \(body)")
            case .noMediaId: return String(localized: "DingTalk did not return a media_id")
            }
        }
    }

    func report(_ r: RunReport) async throws {
        guard var base = URLComponents(string: ReportUpload.webhook) else { throw E.badWebhook }

        // 加签: sign "timestamp\nsecret" and attach both. Must be computed per request.
        if !ReportUpload.secret.isEmpty {
            let ms = Int(now().timeIntervalSince1970 * 1000)
            let stringToSign = "\(ms)\n\(ReportUpload.secret)"
            let mac = HMAC<SHA256>.authenticationCode(for: Data(stringToSign.utf8),
                                                      using: SymmetricKey(data: Data(ReportUpload.secret.utf8)))
            let sign = Data(mac).base64EncodedString()
            base.queryItems = (base.queryItems ?? []) + [
                URLQueryItem(name: "timestamp", value: "\(ms)"),
                URLQueryItem(name: "sign", value: sign),
            ]
        }
        guard let url = base.url else { throw E.badWebhook }

        try await send(url: url, body: markdown(r))

        // Images are separate messages: a robot message carries either text or one image.
        for frame in r.frames.prefix(Self.maxFrames) {
            guard let data = try? Data(contentsOf: frame), !data.isEmpty else { continue }
            if let id = try? await uploadMedia(url: url, data: data) {
                try await send(url: url, body: ["msgtype": "image", "image": ["media_id": id]])
            }
        }
    }

    // MARK: - message bodies

    private func markdown(_ r: RunReport) -> [String: Any] {
        var lines = [
            "### \(r.ok ? "✅" : "⚠️") \(r.task)",
            "",
            "- 结果：\(r.ok ? "完成" : "中断/失败")",
            "- 步骤：\(r.steps)",
        ]
        if let d = r.duration { lines.append("- 耗时：\(d.durationText)") }
        if r.modelCalls > 0 { lines.append("- 模型调用：\(r.modelCalls)") }
        if r.tokens > 0 {
            let t = r.tokens >= 1000 ? "\(Int((Double(r.tokens) / 1000).rounded()))k" : "\(r.tokens)"
            lines.append("- Token：\(t)")
        }
        lines.append("- 时间：\(Self.stamp.string(from: r.date))")
        return ["msgtype": "markdown",
                "markdown": ["title": "iPhone 自动测试", "text": lines.joined(separator: "\n")]]
    }

    private static let stamp: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return f
    }()

    // MARK: - transport

    private func send(url: URL, body: [String: Any]) async throws {
        var req = URLRequest(url: url, timeoutInterval: 30)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try JSONSerialization.data(withJSONObject: body)

        let (data, resp) = try await session.data(for: req)
        let code = (resp as? HTTPURLResponse)?.statusCode ?? -1
        guard code == 200 else { throw E.http(code, String(data: data, encoding: .utf8) ?? "") }
        // DingTalk answers 200 with {"errcode":0,...} even on a rejected robot, so check the body.
        if let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let errcode = json["errcode"] as? Int, errcode != 0 {
            throw E.http(errcode, json["errmsg"] as? String ?? "")
        }
    }

    /// Uploads one image and returns its media_id. Images older than 3 days or over 20 MB are refused.
    private func uploadMedia(url: URL, data: Data) async throws -> String {
        var comp = URLComponents(url: url, resolvingAgainstBaseURL: false)
        comp?.path = "/media/upload"
        comp?.queryItems = (comp?.queryItems ?? []) + [URLQueryItem(name: "type", value: "image")]
        guard let uploadURL = comp?.url else { throw E.badWebhook }

        let boundary = "----iphoneagent\(Int.random(in: 0...1 << 30))"
        var req = URLRequest(url: uploadURL, timeoutInterval: 60)
        req.httpMethod = "POST"
        req.setValue("multipart/form-data; boundary=\(boundary)", forHTTPHeaderField: "Content-Type")

        var body = Data()
        body.append("--\(boundary)\r\n")
        body.append("Content-Disposition: form-data; name=\"media\"; filename=\"frame.jpg\"\r\n")
        body.append("Content-Type: image/jpeg\r\n\r\n")
        body.append(data)
        body.append("\r\n--\(boundary)--\r\n")
        req.httpBody = body

        let (data, resp) = try await session.data(for: req)
        let code = (resp as? HTTPURLResponse)?.statusCode ?? -1
        guard code == 200 else { throw E.http(code, String(data: data, encoding: .utf8) ?? "") }
        guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let id = json["media_id"] as? String, !id.isEmpty else { throw E.noMediaId }
        return id
    }
}
