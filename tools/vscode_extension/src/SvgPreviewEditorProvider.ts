import * as vscode from "vscode";

const MAX_FILE_SIZE = 32 * 1024 * 1024; // 32 MiB

export class SvgPreviewEditorProvider implements vscode.CustomTextEditorProvider {
  public static readonly viewType = "donner.svgPreview";

  constructor(private readonly context: vscode.ExtensionContext) {}

  public async resolveCustomTextEditor(
    document: vscode.TextDocument,
    webviewPanel: vscode.WebviewPanel,
    _token: vscode.CancellationToken,
  ): Promise<void> {
    const mediaDir = vscode.Uri.joinPath(this.context.extensionUri, "media");
    const distWebviewDir = vscode.Uri.joinPath(this.context.extensionUri, "dist", "webview");

    webviewPanel.webview.options = {
      enableScripts: true,
      localResourceRoots: [mediaDir, distWebviewDir],
    };

    webviewPanel.webview.html = this.buildHtml(webviewPanel.webview, distWebviewDir);

    const postMessage = (type: string, body?: unknown) => {
      webviewPanel.webview.postMessage({ type, body });
    };

    const sendDocument = () => {
      const text = document.getText();
      if (Buffer.byteLength(text, "utf-8") > MAX_FILE_SIZE) {
        postMessage("error", "File too large (>32 MiB). Rendering disabled.");
        return;
      }
      postMessage("initDocument", { svgText: text });
    };

    const sendTheme = () => {
      const kind = vscode.window.activeColorTheme.kind;
      postMessage("themeChanged", { kind });
    };

    // Initial state.
    sendDocument();
    sendTheme();

    const changeDocSub = vscode.workspace.onDidChangeTextDocument((e) => {
      if (e.document.uri.toString() === document.uri.toString()) {
        postMessage("setDirtyState", { dirty: true });
      }
    });

    const saveDocSub = vscode.workspace.onDidSaveTextDocument((saved) => {
      if (saved.uri.toString() === document.uri.toString()) {
        const text = saved.getText();
        if (Buffer.byteLength(text, "utf-8") > MAX_FILE_SIZE) {
          postMessage("error", "File too large (>32 MiB). Rendering disabled.");
          return;
        }
        postMessage("replaceDocumentSnapshot", { svgText: text });
      }
    });

    const themeSub = vscode.window.onDidChangeActiveColorTheme(() => {
      sendTheme();
    });

    webviewPanel.onDidDispose(() => {
      changeDocSub.dispose();
      saveDocSub.dispose();
      themeSub.dispose();
    });
  }

  private buildHtml(webview: vscode.Webview, distWebviewDir: vscode.Uri): string {
    const scriptUri = webview.asWebviewUri(vscode.Uri.joinPath(distWebviewDir, "boot.js"));
    const styleUri = webview.asWebviewUri(vscode.Uri.joinPath(distWebviewDir, "style.css"));
    const nonce = getNonce();

    return /* html */ `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <meta http-equiv="Content-Security-Policy"
    content="default-src 'none';
             img-src ${webview.cspSource} data: blob:;
             script-src 'nonce-${nonce}';
             style-src ${webview.cspSource} 'unsafe-inline';
             font-src ${webview.cspSource};">
  <link rel="stylesheet" href="${styleUri}">
  <title>Donner SVG Preview</title>
</head>
<body>
  <div id="toolbar">
    <span id="dirty-badge" class="hidden">Preview reflects last saved version</span>
    <span id="zoom-label">100%</span>
    <button id="zoom-fit" title="Fit to view">Fit</button>
    <button id="zoom-reset" title="Reset zoom">1:1</button>
  </div>
  <div id="canvas-container">
    <canvas id="preview-canvas"></canvas>
  </div>
  <div id="error-overlay" class="hidden">
    <p id="error-message"></p>
  </div>
  <script nonce="${nonce}" src="${scriptUri}"></script>
</body>
</html>`;
  }
}

function getNonce(): string {
  const chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  let nonce = "";
  for (let i = 0; i < 32; i++) {
    nonce += chars.charAt(Math.floor(Math.random() * chars.length));
  }
  return nonce;
}
