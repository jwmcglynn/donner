import * as vscode from "vscode";
import { SvgPreviewEditorProvider } from "./SvgPreviewEditorProvider";

export function activate(context: vscode.ExtensionContext): void {
  const provider = new SvgPreviewEditorProvider(context);

  context.subscriptions.push(
    vscode.window.registerCustomEditorProvider(SvgPreviewEditorProvider.viewType, provider, {
      webviewOptions: { retainContextWhenHidden: true },
      supportsMultipleEditorsPerDocument: false,
    }),
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("donner.openSvgPreview", async () => {
      const activeEditor = vscode.window.activeTextEditor;
      if (!activeEditor || !activeEditor.document.fileName.endsWith(".svg")) {
        vscode.window.showWarningMessage("Open an SVG file first.");
        return;
      }
      await vscode.commands.executeCommand(
        "vscode.openWith",
        activeEditor.document.uri,
        SvgPreviewEditorProvider.viewType,
      );
    }),
  );
}

export function deactivate(): void {}
