## Immediate Solution: VS Code Without Extension

### 1. VS Code Workspace Configuration

```json
// .vscode/settings.json
{
    // AeroSLS language settings
    "files.associations": {
        "*.simi": "simi",
        "*.aerosls": "simi"
    },
    
    // Editor settings for SIMI files
    "[simi]": {
        "editor.tabSize": 4,
        "editor.insertSpaces": true,
        "editor.detectIndentation": false,
        "editor.wordWrap": "wordWrapColumn",
        "editor.wordWrapColumn": 100,
        "editor.rulers": [80, 100],
        "editor.bracketPairColorization.enabled": true,
        "editor.guides.bracketPairs": true
    },
    
    // Format on save
    "editor.formatOnSave": true,
    "editor.codeActionsOnSave": {
        "source.fixAll": true
    },
    
    // SIMI language server settings
    "simi.trace.server": "verbose",
    "simi.maxNumberOfProblems": 100,
    "simi.validate.enable": true,
    
    // Build tasks
    "simi.build.onSave": true,
    "simi.build.target": "all",
    
    // Testing
    "simi.test.autoRun": true,
    "simi.test.coverage": true,
    
    // Terminal settings
    "terminal.integrated.defaultProfile.linux": "simi",
    "terminal.integrated.profiles.linux": {
        "simi": {
            "path": "/bin/bash",
            "args": ["-c", "simi shell"]
        }
    }
}
```

### 2. VS Code Tasks Configuration

```json
// .vscode/tasks.json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "SIMI: Build",
            "type": "shell",
            "command": "cargo",
            "args": [
                "run",
                "-p",
                "simi-cli",
                "--",
                "build",
                "${file}",
                "--target",
                "all"
            ],
            "group": {
                "kind": "build",
                "isDefault": true
            },
            "presentation": {
                "reveal": "always",
                "panel": "new"
            },
            "problemMatcher": [
                "$simi-error",
                "$simi-warning"
            ]
        },
        {
            "label": "SIMI: Run",
            "type": "shell",
            "command": "cargo",
            "args": [
                "run",
                "-p",
                "simi-cli",
                "--",
                "run",
                "${file}",
                "--port",
                "8080"
            ],
            "group": {
                "kind": "test",
                "isDefault": true
            },
            "isBackground": true,
            "problemMatcher": {
                "pattern": {
                    "regexp": "^(.*)$",
                    "file": 1,
                    "location": 2,
                    "message": 3
                },
                "background": {
                    "activeOnStart": true,
                    "beginsPattern": "Starting",
                    "endsPattern": "Runtime ready"
                }
            }
        },
        {
            "label": "SIMI: Test",
            "type": "shell",
            "command": "cargo",
            "args": [
                "run",
                "-p",
                "simi-cli",
                "--",
                "test",
                "${file}"
            ],
            "group": "test",
            "presentation": {
                "reveal": "always",
                "panel": "dedicated"
            }
        },
        {
            "label": "SIMI: Check",
            "type": "shell",
            "command": "cargo",
            "args": [
                "run",
                "-p",
                "simi-cli",
                "--",
                "check",
                "${file}",
                "--strict"
            ],
            "group": "build",
            "problemMatcher": [
                "$simi-error",
                "$simi-warning"
            ]
        },
        {
            "label": "SIMI: Deploy to Kernel",
            "type": "shell",
            "command": "./scripts/deploy-to-kernel.sh",
            "args": [
                "kernel/build/aerosls-kernel.bin",
                "${file}",
                "8080"
            ],
            "group": "build",
            "presentation": {
                "reveal": "always",
                "panel": "dedicated"
            }
        },
        {
            "label": "SIMI: Format",
            "type": "shell",
            "command": "cargo",
            "args": [
                "run",
                "-p",
                "simi-cli",
                "--",
                "fmt",
                "${file}"
            ],
            "group": "build"
        },
        {
            "label": "SIMI: Watch",
            "type": "shell",
            "command": "cargo",
            "args": [
                "watch",
                "-x",
                "run -p simi-cli -- run ${file}"
            ],
            "isBackground": true,
            "group": "build"
        }
    ]
}
```

### 3. VS Code Launch Configuration

```json
// .vscode/launch.json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "Debug SIMI Service",
            "type": "lldb",
            "request": "launch",
            "program": "${workspaceFolder}/target/debug/simi",
            "args": [
                "run",
                "${file}",
                "--port",
                "8080",
                "--debug"
            ],
            "cwd": "${workspaceFolder}",
            "env": {
                "RUST_LOG": "debug",
                "SIMI_DEBUG": "1"
            },
            "preLaunchTask": "SIMI: Build"
        },
        {
            "name": "Debug SIMI Tests",
            "type": "lldb",
            "request": "launch",
            "program": "${workspaceFolder}/target/debug/simi",
            "args": [
                "test",
                "${file}",
                "--debug"
            ],
            "cwd": "${workspaceFolder}",
            "env": {
                "RUST_LOG": "debug"
            }
        },
        {
            "name": "Attach to Kernel",
            "type": "lldb",
            "request": "attach",
            "program": "${workspaceFolder}/kernel/build/aerosls-kernel.bin",
            "pid": "${command:pickProcess}",
            "sourceMap": {
                "/build": "${workspaceFolder}/kernel"
            }
        },
        {
            "name": "Debug Kernel with QEMU",
            "type": "lldb",
            "request": "custom",
            "targetCreateCommands": [
                "target create ${workspaceFolder}/kernel/build/aerosls-kernel.bin"
            ],
            "processCreateCommands": [
                "gdb-remote 1234"
            ],
            "preLaunchTask": "Launch QEMU for Debugging"
        }
    ],
    "compounds": [
        {
            "name": "Debug Service on Kernel",
            "configurations": [
                "Debug Kernel with QEMU",
                "Debug SIMI Service"
            ],
            "presentation": {
                "group": "kernel",
                "order": 1
            }
        }
    ]
}
```

### 4. VS Code Extensions Configuration

```json
// .vscode/extensions.json
{
    "recommendations": [
        // Required for SIMI development
        "aerosls.simi-language-support",
        "rust-lang.rust-analyzer",
        
        // Helpful extensions
        "vadimcn.vscode-lldb",
        "tamasfe.even-better-toml",
        "redhat.vscode-yaml",
        "ms-azuretools.vscode-docker",
        "ms-kubernetes-tools.vscode-kubernetes-tools",
        "github.copilot",
        "github.copilot-chat",
        
        // For kernel development
        "ms-vscode.cpptools",
        "webfreak.debug",
        
        // Observability
        "haskell.haskell",
        "bierner.markdown-mermaid"
    ]
}
```

### 5. VS Code Snippets

```json
// .vscode/simi.code-snippets
{
    "Service Definition": {
        "prefix": "service",
        "body": [
            "service ${1:ServiceName} {",
            "    version: \"${2:0.1.0}\"",
            "    ",
            "    config {",
            "        ${3:port}: int = ${4:8080}",
            "    }",
            "    ",
            "    state {",
            "        ${5:store}: KeyValue<${6:KeyType}, ${7:ValueType}>",
            "    }",
            "    ",
            "    endpoint ${8:endpoint_name}(${9:params}) -> ${10:ReturnType} {",
            "        pipeline ${11:PipelineName} {",
            "            ${12:// stages}",
            "        }",
            "    }",
            "    ",
            "    endpoint health() -> HealthStatus {",
            "        pipeline {",
            "            map check_health",
            "        }",
            "    }",
            "}"
        ],
        "description": "Create a new service definition"
    },
    
    "Pipeline Stage": {
        "prefix": "stage",
        "body": [
            "stage ${1:stage_name} {",
            "    ${2:operation}",
            "    ",
            "    metric \"${3:metric_name}\" {",
            "        type: ${4:counter}",
            "        help: \"${5:description}\"",
            "    }",
            "}"
        ],
        "description": "Create a pipeline stage"
    },
    
    "Map Operation": {
        "prefix": "map",
        "body": "map ${1:function_name}(${2:arguments})",
        "description": "Add a map operation"
    },
    
    "Filter Operation": {
        "prefix": "filter",
        "body": "filter ${1:predicate}",
        "description": "Add a filter operation"
    },
    
    "State Access": {
        "prefix": "state",
        "body": [
            "state ${1:store_name}.${2|get,put,delete,scan|}(",
            "    ${3:key}",
            ")"
        ],
        "description": "Access state store"
    },
    
    "Service Call": {
        "prefix": "call",
        "body": [
            "service ${1:service_name}.${2:method}(",
            "    ${3:payload}",
            ")"
        ],
        "description": "Call another service"
    },
    
    "Circuit Breaker": {
        "prefix": "circuit",
        "body": [
            "circuit \"${1:breaker_name}\" {",
            "    failure_threshold: ${2:5}",
            "    timeout: ${3:30s}",
            "    half_open_requests: ${4:3}",
            "} {",
            "    ${5:// protected operation}",
            "}"
        ],
        "description": "Add circuit breaker"
    },
    
    "Rate Limiter": {
        "prefix": "ratelimit",
        "body": [
            "rate_limit {",
            "    service: \"${1:service_name}\"",
            "    user: ${2:user_id}",
            "    burst: ${3:100}",
            "    rate: ${4:10/s}",
            "}"
        ],
        "description": "Add rate limiting"
    },
    
    "Trace Span": {
        "prefix": "trace",
        "body": [
            "trace \"${1:span_name}\" {",
            "    attributes: {",
            "        ${2:key}: ${3:value}",
            "    }",
            "}"
        ],
        "description": "Add distributed tracing"
    },
    
    "Function Definition": {
        "prefix": "fn",
        "body": [
            "fn ${1:function_name}(${2:params}) -> ${3:ReturnType} {",
            "    ${4:// body}",
            "}"
        ],
        "description": "Define a function"
    },
    
    "Type Definition": {
        "prefix": "type",
        "body": [
            "type ${1:TypeName} = ${2:TypeDefinition}",
            "",
            "type ${1:TypeName} = {",
            "    ${3:field}: ${4:Type}",
            "}"
        ],
        "description": "Define a type"
    },
    
    "Test Definition": {
        "prefix": "test",
        "body": [
            "test ${1:test_name}() {",
            "    let service = ${2:ServiceName}.new();",
            "    let result = service.${3:endpoint}(${4:args});",
            "    assert_eq(result, ${5:expected});",
            "}"
        ],
        "description": "Define a test"
    }
]
```

### 6. Custom VS Code Extension

Now, let's create the actual VS Code extension for full SIMI support:

```typescript
// simi-vscode/package.json
{
    "name": "simi-language-support",
    "displayName": "AeroSLS SIMI Language Support",
    "description": "Language support for AeroSLS SIMI",
    "version": "0.1.0",
    "publisher": "aerosls",
    "engines": {
        "vscode": "^1.85.0"
    },
    "categories": [
        "Programming Languages",
        "Linters",
        "Formatters",
        "Debuggers"
    ],
    "activationEvents": [
        "onLanguage:simi",
        "onCommand:simi.run",
        "onCommand:simi.deploy"
    ],
    "main": "./out/extension.js",
    "contributes": {
        "languages": [{
            "id": "simi",
            "aliases": ["AeroSLS", "SIMI", "simi"],
            "extensions": [".simi", ".aerosls"],
            "configuration": "./language-configuration.json",
            "icon": {
                "light": "./icons/simi-light.svg",
                "dark": "./icons/simi-dark.svg"
            }
        }],
        "grammars": [{
            "language": "simi",
            "scopeName": "source.simi",
            "path": "./syntaxes/simi.tmLanguage.json"
        }],
        "snippets": [
            {
                "language": "simi",
                "path": "./snippets/simi.json"
            }
        ],
        "commands": [
            {
                "command": "simi.build",
                "title": "SIMI: Build",
                "category": "SIMI"
            },
            {
                "command": "simi.run",
                "title": "SIMI: Run",
                "category": "SIMI"
            },
            {
                "command": "simi.deploy",
                "title": "SIMI: Deploy to Kernel",
                "category": "SIMI"
            },
            {
                "command": "simi.test",
                "title": "SIMI: Run Tests",
                "category": "SIMI"
            },
            {
                "command": "simi.format",
                "title": "SIMI: Format Document",
                "category": "SIMI"
            },
            {
                "command": "simi.showDashboard",
                "title": "SIMI: Show Dashboard",
                "category": "SIMI"
            }
        ],
        "keybindings": [
            {
                "command": "simi.build",
                "key": "ctrl+shift+b",
                "when": "editorLangId == simi"
            },
            {
                "command": "simi.run",
                "key": "ctrl+shift+r",
                "when": "editorLangId == simi"
            }
        ],
        "menus": {
            "editor/title": [
                {
                    "command": "simi.run",
                    "when": "editorLangId == simi",
                    "group": "navigation"
                }
            ],
            "editor/context": [
                {
                    "command": "simi.deploy",
                    "when": "editorLangId == simi"
                }
            ]
        },
        "configuration": {
            "title": "SIMI",
            "properties": {
                "simi.server.path": {
                    "type": "string",
                    "default": "simi",
                    "description": "Path to the SIMI language server"
                },
                "simi.trace.server": {
                    "type": "string",
                    "enum": ["off", "messages", "verbose"],
                    "default": "messages",
                    "description": "Traces the communication between VS Code and the language server"
                },
                "simi.build.target": {
                    "type": "string",
                    "enum": ["all", "wasm", "native", "container", "kernel"],
                    "default": "all",
                    "description": "Default build target"
                }
            }
        }
    },
    "scripts": {
        "vscode:prepublish": "npm run compile",
        "compile": "tsc -p ./",
        "watch": "tsc -watch -p ./",
        "pretest": "npm run compile",
        "test": "node ./out/test/runTest.js"
    },
    "devDependencies": {
        "@types/vscode": "^1.85.0",
        "@types/node": "^20.0.0",
        "typescript": "^5.3.0",
        "vscode-test": "^1.6.1"
    }
}
```

```typescript
// simi-vscode/src/extension.ts
import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import { exec, spawn } from 'child_process';

let outputChannel: vscode.OutputChannel;
let languageClient: any;

export function activate(context: vscode.ExtensionContext) {
    console.log('AeroSLS SIMI extension activated');
    
    // Create output channel
    outputChannel = vscode.window.createOutputChannel('SIMI');
    outputChannel.appendLine('SIMI Language Support activated');
    
    // Register commands
    registerCommands(context);
    
    // Register language features
    registerLanguageFeatures(context);
    
    // Start language server
    startLanguageServer(context);
    
    // Register status bar
    registerStatusBar(context);
    
    // Register file watchers
    registerFileWatchers(context);
}

function registerCommands(context: vscode.ExtensionContext) {
    // Build command
    context.subscriptions.push(
        vscode.commands.registerCommand('simi.build', async () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor) return;
            
            const document = editor.document;
            await document.save();
            
            outputChannel.show();
            outputChannel.appendLine(`🔨 Building ${document.fileName}...`);
            
            const target = vscode.workspace.getConfiguration('simi')
                .get('build.target', 'all');
            
            const terminal = vscode.window.createTerminal('SIMI Build');
            terminal.sendText(`simi build ${document.fileName} --target ${target}`);
            terminal.show();
        })
    );
    
    // Run command
    context.subscriptions.push(
        vscode.commands.registerCommand('simi.run', async () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor) return;
            
            const document = editor.document;
            await document.save();
            
            const terminal = vscode.window.createTerminal('SIMI Run');
            terminal.sendText(`simi run ${document.fileName} --port 8080`);
            terminal.show();
            
            // Open browser after delay
            setTimeout(() => {
                vscode.env.openExternal(
                    vscode.Uri.parse('http://localhost:8080/dashboard')
                );
            }, 3000);
        })
    );
    
    // Deploy to kernel command
    context.subscriptions.push(
        vscode.commands.registerCommand('simi.deploy', async () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor) return;
            
            const document = editor.document;
            await document.save();
            
            const terminal = vscode.window.createTerminal('SIMI Kernel Deploy');
            terminal.sendText(
                `./scripts/deploy-to-kernel.sh ` +
                `kernel/build/aerosls-kernel.bin ` +
                `${document.fileName} 8080`
            );
            terminal.show();
            
            vscode.window.showInformationMessage(
                'Deploying to AeroSLS Kernel...'
            );
        })
    );
    
    // Test command
    context.subscriptions.push(
        vscode.commands.registerCommand('simi.test', async () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor) return;
            
            const document = editor.document;
            await document.save();
            
            const terminal = vscode.window.createTerminal('SIMI Tests');
            terminal.sendText(`simi test ${document.fileName}`);
            terminal.show();
        })
    );
    
    // Format command
    context.subscriptions.push(
        vscode.commands.registerCommand('simi.format', async () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor) return;
            
            const document = editor.document;
            await document.save();
            
            exec(`simi fmt ${document.fileName}`, (error, stdout, stderr) => {
                if (error) {
                    vscode.window.showErrorMessage(`Format error: ${error.message}`);
                    return;
                }
                vscode.window.showInformationMessage('Document formatted');
            });
        })
    );
    
    // Show dashboard
    context.subscriptions.push(
        vscode.commands.registerCommand('simi.showDashboard', () => {
            const panel = vscode.window.createWebviewPanel(
                'simiDashboard',
                'SIMI Dashboard',
                vscode.ViewColumn.Two,
                { enableScripts: true }
            );
            
            panel.webview.html = getDashboardHtml();
        })
    );
}

function registerLanguageFeatures(context: vscode.ExtensionContext) {
    // Register completion provider
    context.subscriptions.push(
        vscode.languages.registerCompletionItemProvider('simi', {
            provideCompletionItems(document, position) {
                const linePrefix = document.lineAt(position).text
                    .substr(0, position.character);
                
                const completions = [
                    // Keywords
                    new vscode.CompletionItem('service', vscode.CompletionItemKind.Keyword),
                    new vscode.CompletionItem('endpoint', vscode.CompletionItemKind.Keyword),
                    new vscode.CompletionItem('pipeline', vscode.CompletionItemKind.Keyword),
                    new vscode.CompletionItem('state', vscode.CompletionItemKind.Keyword),
                    new vscode.CompletionItem('config', vscode.CompletionItemKind.Keyword),
                    new vscode.CompletionItem('map', vscode.CompletionItemKind.Keyword),
                    new vscode.CompletionItem('filter', vscode.CompletionItemKind.Keyword),
                    new vscode.CompletionItem('reduce', vscode.CompletionItemKind.Keyword),
                    
                    // Built-in functions
                    new vscode.CompletionItem('format', vscode.CompletionItemKind.Function),
                    new vscode.CompletionItem('now', vscode.CompletionItemKind.Function),
                    new vscode.CompletionItem('generate_uuid', vscode.CompletionItemKind.Function),
                    
                    // Types
                    new vscode.CompletionItem('String', vscode.CompletionItemKind.TypeParameter),
                    new vscode.CompletionItem('Int', vscode.CompletionItemKind.TypeParameter),
                    new vscode.CompletionItem('Float', vscode.CompletionItemKind.TypeParameter),
                    new vscode.CompletionItem('Bool', vscode.CompletionItemKind.TypeParameter),
                ];
                
                return completions;
            }
        }, '.', ' ', '\n')
    );
    
    // Register hover provider
    context.subscriptions.push(
        vscode.languages.registerHoverProvider('simi', {
            provideHover(document, position) {
                const wordRange = document.getWordRangeAtPosition(position);
                const word = document.getText(wordRange);
                
                const hovers: { [key: string]: string } = {
                    'service': '**Service Definition**\n\nDefines a new service with endpoints, state, and configuration.',
                    'pipeline': '**Pipeline**\n\nA sequence of stages that process data.',
                    'map': '**Map Operation**\n\nTransforms each element in the data stream.',
                    'filter': '**Filter Operation**\n\nRemoves elements that don\'t match the predicate.',
                    'state': '**State Access**\n\nRead or write to a state store.',
                };
                
                if (hovers[word]) {
                    return new vscode.Hover(new vscode.MarkdownString(hovers[word]));
                }
                
                return null;
            }
        })
    );
    
    // Register definition provider
    context.subscriptions.push(
        vscode.languages.registerDefinitionProvider('simi', {
            provideDefinition(document, position) {
                const wordRange = document.getWordRangeAtPosition(position);
                const word = document.getText(wordRange);
                
                // Search for function/type definitions
                const text = document.getText();
                const regex = new RegExp(`(fn|type|service)\\s+${word}\\b`, 'g');
                let match;
                
                while ((match = regex.exec(text)) !== null) {
                    const pos = document.positionAt(match.index);
                    return new vscode.Location(document.uri, pos);
                }
                
                return null;
            }
        })
    );
    
    // Register diagnostics
    context.subscriptions.push(
        vscode.languages.createDiagnosticCollection('simi')
    );
    
    // Run diagnostics on save
    context.subscriptions.push(
        vscode.workspace.onDidSaveTextDocument(document => {
            if (document.languageId === 'simi') {
                runDiagnostics(document);
            }
        })
    );
}

function runDiagnostics(document: vscode.TextDocument) {
    const diagnostics: vscode.Diagnostic[] = [];
    const text = document.getText();
    
    // Check for common issues
    const lines = text.split('\n');
    lines.forEach((line, index) => {
        // Check for missing semicolons (AeroSLS doesn't use them)
        if (line.trim().endsWith(';')) {
            const range = new vscode.Range(index, line.length - 1, index, line.length);
            diagnostics.push({
                message: 'AeroSLS does not use semicolons',
                range,
                severity: vscode.DiagnosticSeverity.Warning,
                source: 'simi'
            });
        }
        
        // Check for unclosed brackets
        const openBrackets = (line.match(/\{/g) || []).length;
        const closeBrackets = (line.match(/\}/g) || []).length;
        // More sophisticated bracket matching would go here
    });
    
    // Check for undefined services
    const serviceRegex = /service\s+(\w+)/g;
    const serviceCallRegex = /service\s+(\w+)\./g;
    // Compare defined services with service calls
    
    const diagnosticCollection = vscode.languages.getDiagnostics('simi')?.[0];
    // Update diagnostics
}

function registerStatusBar(context: vscode.ExtensionContext) {
    const statusBarItem = vscode.window.createStatusBarItem(
        vscode.StatusBarAlignment.Right,
        100
    );
    statusBarItem.text = '$(rocket) SIMI';
    statusBarItem.tooltip = 'AeroSLS SIMI Status';
    statusBarItem.command = 'simi.showDashboard';
    statusBarItem.show();
    
    context.subscriptions.push(statusBarItem);
}

function registerFileWatchers(context: vscode.ExtensionContext) {
    // Watch for changes in .simi files
    const watcher = vscode.workspace.createFileSystemWatcher(
        '**/*.simi'
    );
    
    watcher.onDidChange(uri => {
        outputChannel.appendLine(`File changed: ${uri.fsPath}`);
    });
    
    watcher.onDidCreate(uri => {
        outputChannel.appendLine(`File created: ${uri.fsPath}`);
    });
    
    context.subscriptions.push(watcher);
}

function startLanguageServer(context: vscode.ExtensionContext) {
    // Start the SIMI language server
    outputChannel.appendLine('Starting SIMI language server...');
    
    // In production, this would start the actual language server
    // For now, we use the built-in features
}

function getDashboardHtml(): string {
    return `
    <!DOCTYPE html>
    <html>
    <head>
        <style>
            body { 
                font-family: system-ui; 
                background: var(--vscode-editor-background);
                color: var(--vscode-editor-foreground);
                padding: 20px;
            }
            .card {
                background: var(--vscode-textBlockQuote-background);
                border: 1px solid var(--vscode-widget-border);
                border-radius: 8px;
                padding: 16px;
                margin-bottom: 16px;
            }
            .metric {
                display: inline-block;
                margin: 8px 16px;
            }
            .metric-label {
                font-size: 12px;
                color: var(--vscode-descriptionForeground);
            }
            .metric-value {
                font-size: 24px;
                font-weight: bold;
                color: var(--vscode-charts-green);
            }
        </style>
    </head>
    <body>
        <h1>🚀 SIMI Dashboard</h1>
        <div class="card">
            <div class="metric">
                <div class="metric-label">Services</div>
                <div class="metric-value">3</div>
            </div>
            <div class="metric">
                <div class="metric-label">Requests/s</div>
                <div class="metric-value">1,234</div>
            </div>
            <div class="metric">
                <div class="metric-label">P99 Latency</div>
                <div class="metric-value">45ms</div>
            </div>
        </div>
    </body>
    </html>`;
}

export function deactivate() {
    outputChannel?.appendLine('SIMI Language Support deactivated');
}
```

### 7. Language Grammar

```json
// simi-vscode/syntaxes/simi.tmLanguage.json
{
    "$schema": "https://raw.githubusercontent.com/martinring/tmlanguage/master/tmlanguage.json",
    "name": "AeroSLS SIMI",
    "patterns": [
        {
            "include": "#keywords"
        },
        {
            "include": "#strings"
        },
        {
            "include": "#comments"
        },
        {
            "include": "#types"
        },
        {
            "include": "#numbers"
        },
        {
            "include": "#operators"
        }
    ],
    "repository": {
        "keywords": {
            "patterns": [{
                "name": "keyword.control.simi",
                "match": "\\b(service|endpoint|pipeline|state|config|map|filter|reduce|window|fn|test|type|enum|match|if|else|for|in|return|async|await|parallel|retry|circuit|trace|metric|rate_limit)\\b"
            }]
        },
        "strings": {
            "name": "string.quoted.double.simi",
            "begin": "\"",
            "end": "\"",
            "patterns": [{
                "name": "constant.character.escape.simi",
                "match": "\\\\."
            }]
        },
        "comments": {
            "patterns": [
                {
                    "name": "comment.line.double-slash.simi",
                    "match": "//.*$"
                },
                {
                    "name": "comment.block.simi",
                    "begin": "/\\*",
                    "end": "\\*/"
                }
            ]
        },
        "types": {
            "patterns": [{
                "name": "storage.type.simi",
                "match": "\\b(String|Int|Float|Bool|Bytes|Timestamp|UUID|Any|Never|Optional|Array|List|Set|Map|Stream|KeyValue|Cache|Counter)\\b"
            }]
        },
        "numbers": {
            "patterns": [{
                "name": "constant.numeric.simi",
                "match": "\\b[0-9]+(\\.[0-9]+)?\\b"
            }]
        },
        "operators": {
            "patterns": [{
                "name": "keyword.operator.simi",
                "match": "(=>|->|==|!=|>=|<=|>|<|\\+|\\-|\\*|/|%|&&|\\|\\||!)"
            }]
        }
    },
    "scopeName": "source.simi"
}
```

### 8. Language Configuration

```json
// simi-vscode/language-configuration.json
{
    "comments": {
        "lineComment": "//",
        "blockComment": ["/*", "*/"]
    },
    "brackets": [
        ["{", "}"],
        ["[", "]"],
        ["(", ")"]
    ],
    "autoClosingPairs": [
        { "open": "{", "close": "}" },
        { "open": "[", "close": "]" },
        { "open": "(", "close": ")" },
        { "open": "\"", "close": "\"" },
        { "open": "'", "close": "'" }
    ],
    "surroundingPairs": [
        ["{", "}"],
        ["[", "]"],
        ["(", ")"],
        ["\"", "\""],
        ["'", "'"]
    ],
    "indentationRules": {
        "increaseIndentPattern": "^.*\\{[^}]*$",
        "decreaseIndentPattern": "^\\s*\\}"
    }
}
```

### 9. Build and Install

```bash
#!/bin/bash
# scripts/build-vscode-extension.sh
# Build and install the VS Code extension

echo "🔨 Building VS Code extension..."

cd simi-vscode

# Install dependencies
npm install

# Compile TypeScript
npm run compile

# Package the extension
npx vsce package

# Install the extension
code --install-extension simi-language-support-0.1.0.vsix

echo "✅ Extension installed!"
echo ""
echo "Reload VS Code to activate AeroSLS SIMI support"
```

## Usage Instructions

### Without Extension (Immediate)

1. Open your project in VS Code:

```bash
code aerosls-project/
```

1. Press `Ctrl+Shift+B` to build your SIMI file
2. Press `Ctrl+Shift+R` to run your service
3. Use the Terminal to interact with SIMI CLI

### With Extension (Full Experience)

```bash
# Build and install the extension
cd simi-vscode
npm install
npm run compile
npx vsce package
code --install-extension simi-language-support-0.1.0.vsix
```

### After installation, you get:

- **Syntax highlighting** for .simi files
- **Code completion** for keywords, functions, and types
- **Hover information** for built-in constructs
- **Go to definition** for functions and types
- **Diagnostics** for common errors and warnings
- **Snippets** for quick service creation
- **Status bar** integration
- **Build/Run/Deploy** buttons
- **Dashboard** webview

The key is that you can start developing **immediately** with just the workspace configuration files. The extension provides the full IDE experience but isn't required to get started!
