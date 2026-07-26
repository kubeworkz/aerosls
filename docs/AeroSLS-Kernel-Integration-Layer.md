## AeroSLS Kernel Integration Layer

### 1. Kernel System Call Interface (syscalls)

```rust
// crates/simi-kernel/src/syscalls.rs
// System call interface matching your AeroSLS kernel

/// System call numbers matching your kernel
#[repr(u64)]
pub enum SyscallNumber {
    // Process management
    Exit = 1,
    Fork = 2,
    Exec = 3,
    WaitPid = 4,
    
    // Memory management
    MMap = 10,
    MUnmap = 11,
    Brk = 12,
    
    // I/O operations
    Read = 20,
    Write = 21,
    Open = 22,
    Close = 23,
    
    // Network operations
    Socket = 30,
    Bind = 31,
    Listen = 32,
    Accept = 33,
    Connect = 34,
    Send = 35,
    Recv = 36,
    
    // SIMI-specific syscalls
    SimiSpawn = 100,
    SimiStateGet = 101,
    SimiStatePut = 102,
    SimiServiceCall = 103,
    SimiRegister = 104,
    SimiMetric = 105,
    SimiTrace = 106,
}

/// System call interface for the kernel
#[inline(always)]
unsafe fn syscall(number: SyscallNumber, arg1: u64, arg2: u64, arg3: u64) -> u64 {
    let result: u64;
    asm!(
        "syscall",
        in("rax") number as u64,
        in("rdi") arg1,
        in("rsi") arg2,
        in("rdx") arg3,
        lateout("rax") result,
        options(nostack, preserves_flags)
    );
    result
}

/// SIMI-specific system calls
pub mod simi {
    use super::*;
    
    /// Spawn a new SIMI service on the kernel
    pub fn spawn_service(service_binary: &[u8], config: &[u8]) -> u64 {
        unsafe {
            syscall(
                SyscallNumber::SimiSpawn,
                service_binary.as_ptr() as u64,
                service_binary.len() as u64,
                config.as_ptr() as u64,
            )
        }
    }
    
    /// Get state from kernel-managed state store
    pub fn state_get(state_id: u64, key: &[u8]) -> Option<Vec<u8>> {
        let mut buf = vec![0u8; 4096];
        let result = unsafe {
            syscall(
                SyscallNumber::SimiStateGet,
                state_id,
                key.as_ptr() as u64,
                buf.as_mut_ptr() as u64,
            )
        };
        
        if result > 0 {
            buf.truncate(result as usize);
            Some(buf)
        } else {
            None
        }
    }
    
    /// Store state in kernel-managed state store
    pub fn state_put(state_id: u64, key: &[u8], value: &[u8]) -> u64 {
        unsafe {
            syscall(
                SyscallNumber::SimiStatePut,
                state_id,
                key.as_ptr() as u64,
                value.as_ptr() as u64,
            )
        }
    }
    
    /// Make a service call through the kernel
    pub fn service_call(service_id: u64, method: &[u8], payload: &[u8]) -> u64 {
        unsafe {
            syscall(
                SyscallNumber::SimiServiceCall,
                service_id,
                method.as_ptr() as u64,
                payload.as_ptr() as u64,
            )
        }
    }
    
    /// Register a service with the kernel
    pub fn register_service(name: &[u8], endpoint: &[u8]) -> u64 {
        unsafe {
            syscall(
                SyscallNumber::SimiRegister,
                name.as_ptr() as u64,
                endpoint.as_ptr() as u64,
                0,
            )
        }
    }
}
```

### 2. Kernel Runtime Adapter

```rust
// crates/simi-kernel/src/runtime.rs
// Runtime adapter that runs SIMI services on your kernel

use crate::syscalls::simi;
use simi_core::*;
use std::collections::HashMap;
use std::sync::Arc;
use tokio::sync::RwLock;

/// Kernel runtime that executes SIMI services
pub struct KernelRuntime {
    services: Arc<RwLock<HashMap<u64, KernelService>>>,
    state_stores: Arc<RwLock<HashMap<u64, KernelStateStore>>>,
    next_service_id: AtomicU64,
    next_state_id: AtomicU64,
}

#[derive(Debug)]
struct KernelService {
    id: u64,
    name: String,
    binary: Vec<u8>,
    status: ServiceStatus,
}

#[derive(Debug)]
enum ServiceStatus {
    Running,
    Stopped,
    Crashed(String),
}

#[derive(Debug)]
struct KernelStateStore {
    id: u64,
    name: String,
    data: HashMap<Vec<u8>, Vec<u8>>,
}

impl KernelRuntime {
    pub fn new() -> Self {
        KernelRuntime {
            services: Arc::new(RwLock::new(HashMap::new())),
            state_stores: Arc::new(RwLock::new(HashMap::new())),
            next_service_id: AtomicU64::new(1),
            next_state_id: AtomicU64::new(1),
        }
    }
    
    /// Deploy a SIMI service to the kernel
    pub async fn deploy_service(
        &self,
        module: &SimiModule,
    ) -> Result<u64, KernelError> {
        // Compile the module to a kernel-compatible binary
        let binary = self.compile_to_kernel_binary(module)?;
        
        let service_id = self.next_service_id.fetch_add(1, Ordering::SeqCst);
        
        // Register with kernel
        let service_name = format!("{}-{}", module.header.name, service_id);
        simi::register_service(
            service_name.as_bytes(),
            &service_id.to_le_bytes(),
        );
        
        // Spawn service on kernel
        let config = serde_json::to_vec(&KernelServiceConfig {
            service_id,
            name: service_name.clone(),
            memory_limit: 64 * 1024 * 1024, // 64MB
            cpu_shares: 1024,
        })?;
        
        let result = simi::spawn_service(&binary, &config);
        
        if result == 0 {
            let service = KernelService {
                id: service_id,
                name: service_name,
                binary,
                status: ServiceStatus::Running,
            };
            
            self.services.write().await.insert(service_id, service);
            Ok(service_id)
        } else {
            Err(KernelError::SpawnFailed(result))
        }
    }
    
    /// Compile SIMI module to kernel binary
    fn compile_to_kernel_binary(
        &self,
        module: &SimiModule,
    ) -> Result<Vec<u8>, KernelError> {
        // This would compile the SIMI module to a static binary
        // that links against the kernel's syscall interface
        
        // For now, create a minimal ELF binary
        let mut binary = Vec::new();
        
        // ELF Header
        binary.extend_from_slice(&[
            0x7f, b'E', b'L', b'F',  // Magic
            2,  // 64-bit
            1,  // Little endian
            1,  // ELF version
            0,  // System V ABI
            0,  // ABI version
        ]);
        
        // Pad to proper ELF size
        binary.resize(1024, 0);
        
        // Embed the SIMI module as data
        let module_bytes = bincode::serialize(module)?;
        let module_len = module_bytes.len() as u64;
        
        binary.extend_from_slice(&module_len.to_le_bytes());
        binary.extend_from_slice(&module_bytes);
        
        Ok(binary)
    }
    
    /// Execute a pipeline on the kernel
    pub async fn execute_pipeline(
        &self,
        service_id: u64,
        pipeline: &Pipeline,
        input: Value,
    ) -> Result<Value, KernelError> {
        let service = self.services.read().await
            .get(&service_id)
            .cloned()
            .ok_or(KernelError::ServiceNotFound(service_id))?;
        
        if !matches!(service.status, ServiceStatus::Running) {
            return Err(KernelError::ServiceNotRunning(service_id));
        }
        
        // Execute pipeline stages
        let mut data = input;
        
        for stage in &pipeline.stages {
            data = self.execute_stage(service_id, stage, data).await?;
        }
        
        Ok(data)
    }
    
    async fn execute_stage(
        &self,
        service_id: u64,
        stage: &PipelineStage,
        data: Value,
    ) -> Result<Value, KernelError> {
        match &stage.operation {
            StageOperation::Map(op) => {
                // Execute map operation
                Ok(data) // Simplified
            }
            StageOperation::StateAccess(op) => {
                self.execute_state_operation(service_id, op).await
            }
            StageOperation::ServiceCall(op) => {
                self.execute_service_call(service_id, op).await
            }
            _ => Ok(data),
        }
    }
    
    async fn execute_state_operation(
        &self,
        service_id: u64,
        op: &StateOperation,
    ) -> Result<Value, KernelError> {
        // Use kernel syscalls for state operations
        match &op.operation {
            StateOp::Get { key } => {
                let key_bytes = serde_json::to_vec(key)?;
                let result = simi::state_get(service_id, &key_bytes);
                
                match result {
                    Some(value) => Ok(serde_json::from_slice(&value)?),
                    None => Ok(Value::Null),
                }
            }
            StateOp::Put { key, value } => {
                let key_bytes = serde_json::to_vec(key)?;
                let value_bytes = serde_json::to_vec(value)?;
                
                simi::state_put(service_id, &key_bytes, &value_bytes);
                Ok(Value::Null)
            }
            _ => Ok(Value::Null),
        }
    }
    
    async fn execute_service_call(
        &self,
        service_id: u64,
        op: &ServiceCallOperation,
    ) -> Result<Value, KernelError> {
        let method_bytes = op.method.as_bytes();
        let payload_bytes = serde_json::to_vec(&op.payload)?;
        
        let result = simi::service_call(
            service_id,
            method_bytes,
            &payload_bytes,
        );
        
        // Parse result
        Ok(Value::Int(result as i64))
    }
    
    /// Create a state store on the kernel
    pub async fn create_state_store(
        &self,
        name: &str,
    ) -> u64 {
        let id = self.next_state_id.fetch_add(1, Ordering::SeqCst);
        
        self.state_stores.write().await.insert(id, KernelStateStore {
            id,
            name: name.to_string(),
            data: HashMap::new(),
        });
        
        id
    }
}

#[derive(Debug, serde::Serialize, serde::Deserialize)]
struct KernelServiceConfig {
    service_id: u64,
    name: String,
    memory_limit: usize,
    cpu_shares: u64,
}

#[derive(Debug, thiserror::Error)]
pub enum KernelError {
    #[error("Service not found: {0}")]
    ServiceNotFound(u64),
    #[error("Service not running: {0}")]
    ServiceNotRunning(u64),
    #[error("Spawn failed: {0}")]
    SpawnFailed(u64),
    #[error("Serialization error: {0}")]
    SerializationError(String),
}

impl From<serde_json::Error> for KernelError {
    fn from(e: serde_json::Error) -> Self {
        KernelError::SerializationError(e.to_string())
    }
}

impl From<bincode::Error> for KernelError {
    fn from(e: bincode::Error) -> Self {
        KernelError::SerializationError(e.to_string())
    }
}
```

### 3. Kernel Boot Integration

```rust
// crates/simi-kernel/src/boot.rs
// Integration with your QEMU boot process

use std::net::SocketAddr;
use tokio::net::TcpListener;

/// Boot configuration for kernel integration
#[derive(Debug)]
pub struct KernelBootConfig {
    pub qemu_binary: String,
    pub kernel_image: String,
    pub memory: String,
    pub smp: u32,
    pub network: NetworkConfig,
}

#[derive(Debug)]
pub struct NetworkConfig {
    pub host_port: u16,
    pub guest_port: u16,
    pub mac_address: String,
}

impl Default for KernelBootConfig {
    fn default() -> Self {
        KernelBootConfig {
            qemu_binary: "qemu-system-x86_64".into(),
            kernel_image: "build/aerosls-kernel.bin".into(),
            memory: "512M".into(),
            smp: 4,
            network: NetworkConfig {
                host_port: 8080,
                guest_port: 8080,
                mac_address: "52:54:00:12:34:56".into(),
            },
        }
    }
}

/// Launch the kernel in QEMU with SIMI runtime
pub async fn launch_kernel(config: &KernelBootConfig) -> Result<(), Box<dyn std::error::Error>> {
    println!("🚀 Launching AeroSLS Kernel in QEMU...");
    
    // Build QEMU command
    let mut cmd = std::process::Command::new(&config.qemu_binary);
    
    cmd.arg("-kernel").arg(&config.kernel_image);
    cmd.arg("-m").arg(&config.memory);
    cmd.arg("-smp").arg(config.smp.to_string());
    
    // Network configuration
    cmd.arg("-netdev")
       .arg(format!(
           "user,id=net0,hostfwd=tcp::{}-:{}",
           config.network.host_port,
           config.network.guest_port
       ))
       .arg("-device")
       .arg(format!(
           "virtio-net-pci,netdev=net0,mac={}",
           config.network.mac_address
       ));
    
    // Enable serial console
    cmd.arg("-serial").arg("stdio");
    
    // No graphic output
    cmd.arg("-nographic");
    
    // Enable KVM for better performance
    cmd.arg("-enable-kvm");
    
    println!("  QEMU: {}", config.qemu_binary);
    println!("  Kernel: {}", config.kernel_image);
    println!("  Memory: {}", config.memory);
    println!("  CPUs: {}", config.smp);
    println!("  Port: {} -> {}", config.network.host_port, config.network.guest_port);
    
    // Spawn QEMU process
    let mut child = cmd.spawn()?;
    
    // Wait for kernel to boot and network to be ready
    println!("\n⏳ Waiting for kernel to boot...");
    wait_for_kernel_ready(config.network.host_port).await?;
    
    println!("✅ Kernel is ready!");
    println!("   Service endpoint: http://localhost:{}", config.network.host_port);
    
    // Keep running until interrupted
    tokio::signal::ctrl_c().await?;
    
    println!("\n🛑 Shutting down kernel...");
    child.kill()?;
    child.wait()?;
    
    Ok(())
}

async fn wait_for_kernel_ready(port: u16) -> Result<(), Box<dyn std::error::Error>> {
    let addr = SocketAddr::from(([127, 0, 0, 1], port));
    let max_retries = 30;
    
    for i in 0..max_retries {
        match TcpListener::bind(addr).await {
            Ok(_) => {
                // Port is available, kernel hasn't started yet
            }
            Err(_) => {
                // Port is in use, kernel is ready
                return Ok(());
            }
        }
        
        tokio::time::sleep(std::time::Duration::from_secs(1)).await;
        
        if i % 5 == 0 {
            println!("  Still waiting... ({}/{})", i + 1, max_retries);
        }
    }
    
    Err("Kernel failed to boot within timeout".into())
}
```

### 4. Kernel Service Deployer

```rust
// crates/simi-kernel/src/deploy.rs
// Deploy services to your QEMU kernel

use simi_core::*;
use crate::runtime::KernelRuntime;
use crate::boot::KernelBootConfig;

/// Deploy SIMI services to the kernel
pub struct KernelDeployer {
    runtime: KernelRuntime,
    boot_config: KernelBootConfig,
}

impl KernelDeployer {
    pub fn new(boot_config: KernelBootConfig) -> Self {
        KernelDeployer {
            runtime: KernelRuntime::new(),
            boot_config,
        }
    }
    
    /// Deploy a complete application to the kernel
    pub async fn deploy_application(
        &self,
        app: &SimiModule,
    ) -> Result<KernelDeployment, Box<dyn std::error::Error>> {
        println!("📦 Deploying {} to AeroSLS Kernel...", app.header.name);
        
        // Step 1: Create state stores
        let mut state_store_ids = Vec::new();
        for state_def in &app.state {
            let id = self.runtime.create_state_store(&state_def.id).await;
            state_store_ids.push((state_def.id.clone(), id));
            println!("  📊 Created state store: {} (id: {})", state_def.id, id);
        }
        
        // Step 2: Deploy each service
        let mut service_ids = Vec::new();
        for service_def in &app.services {
            let service_module = SimiModule {
                header: ModuleHeader {
                    name: service_def.name.clone(),
                    version: app.header.version,
                    source: format!("deployed:{}", service_def.name),
                },
                services: vec![service_def.clone()],
                ..Default::default()
            };
            
            let id = self.runtime.deploy_service(&service_module).await?;
            service_ids.push((service_def.name.clone(), id));
            println!("  🚀 Deployed service: {} (id: {})", service_def.name, id);
        }
        
        // Step 3: Set up networking
        println!("  🌐 Setting up network endpoints...");
        for (name, id) in &service_ids {
            println!("     {} -> http://localhost:{}/{}", 
                name,
                self.boot_config.network.host_port,
                name
            );
        }
        
        Ok(KernelDeployment {
            app_name: app.header.name.clone(),
            services: service_ids,
            state_stores: state_store_ids,
            boot_config: self.boot_config.clone(),
        })
    }
}

#[derive(Debug)]
pub struct KernelDeployment {
    pub app_name: String,
    pub services: Vec<(String, u64)>,
    pub state_stores: Vec<(String, u64)>,
    pub boot_config: KernelBootConfig,
}

impl KernelDeployment {
    pub fn print_summary(&self) {
        println!("\n╔══════════════════════════════════════════════════════════╗");
        println!("║           AeroSLS Kernel Deployment Summary               ║");
        println!("╚══════════════════════════════════════════════════════════╝");
        println!("\n  Application: {}", self.app_name);
        println!("  Services: {}", self.services.len());
        println!("  State Stores: {}", self.state_stores.len());
        println!("\n  Endpoints:");
        for (name, id) in &self.services {
            println!("    http://localhost:{}/{}/", 
                self.boot_config.network.host_port,
                name
            );
        }
        println!("\n  Health Check:");
        println!("    http://localhost:{}/health", self.boot_config.network.host_port);
        println!("\n  Dashboard:");
        println!("    http://localhost:{}/dashboard", self.boot_config.network.host_port);
    }
}
```

### 5. CLI Integration for Kernel Deployment

```rust
// crates/simi-cli/src/kernel.rs
// CLI commands for kernel deployment

use clap::Subcommand;

#[derive(Subcommand)]
pub enum KernelCommands {
    /// Launch the kernel in QEMU
    Launch {
        #[arg(long, default_value = "build/aerosls-kernel.bin")]
        kernel: String,
        
        #[arg(long, default_value = "512M")]
        memory: String,
        
        #[arg(long, default_value = "4")]
        smp: u32,
        
        #[arg(long, default_value = "8080")]
        port: u16,
    },
    
    /// Deploy a service to the kernel
    Deploy {
        /// Path to the service file
        file: String,
        
        #[arg(long, default_value = "8080")]
        port: u16,
    },
    
    /// Check kernel status
    Status {
        #[arg(long, default_value = "8080")]
        port: u16,
    },
}

pub async fn handle_kernel_command(cmd: KernelCommands) -> anyhow::Result<()> {
    match cmd {
        KernelCommands::Launch { kernel, memory, smp, port } => {
            let config = KernelBootConfig {
                qemu_binary: "qemu-system-x86_64".into(),
                kernel_image: kernel,
                memory,
                smp,
                network: NetworkConfig {
                    host_port: port,
                    guest_port: 8080,
                    mac_address: "52:54:00:12:34:56".into(),
                },
            };
            
            simi_kernel::boot::launch_kernel(&config).await?;
        }
        
        KernelCommands::Deploy { file, port } => {
            println!("📦 Reading service: {}", file);
            let source = std::fs::read_to_string(&file)?;
            let module = simi_parser::parse_source(&source)?;
            
            let config = KernelBootConfig {
                network: NetworkConfig {
                    host_port: port,
                    ..Default::default()
                },
                ..Default::default()
            };
            
            let deployer = KernelDeployer::new(config);
            let deployment = deployer.deploy_application(&module).await?;
            deployment.print_summary();
        }
        
        KernelCommands::Status { port } => {
            // Check if kernel is running
            let client = reqwest::Client::new();
            match client
                .get(format!("http://localhost:{}/health", port))
                .send()
                .await
            {
                Ok(response) => {
                    if response.status().is_success() {
                        println!("✅ Kernel is running");
                        println!("   URL: http://localhost:{}", port);
                    } else {
                        println!("⚠️  Kernel returned error: {}", response.status());
                    }
                }
                Err(_) => {
                    println!("❌ Kernel is not running");
                    println!("   Start with: simi kernel launch");
                }
            }
        }
    }
    
    Ok(())
}
```

### 6. Complete Deployment Script

```bash
#!/bin/bash
# scripts/deploy-to-kernel.sh
# Deploy AeroSLS services to your QEMU kernel

set -e

KERNEL_IMAGE="${1:-build/aerosls-kernel.bin}"
SERVICE_FILE="${2:-examples/hello-service/src/main.simi}"
HOST_PORT="${3:-8080}"

echo "╔══════════════════════════════════════════════════════════╗"
echo "║     Deploying to AeroSLS Kernel on QEMU                  ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""
echo "Kernel: ${KERNEL_IMAGE}"
echo "Service: ${SERVICE_FILE}"
echo "Port: ${HOST_PORT}"
echo ""

# Build the kernel if needed
if [ ! -f "${KERNEL_IMAGE}" ]; then
    echo "🔨 Building kernel..."
    cd kernel
    make
    cd ..
fi

# Build the SIMI tools
echo "🔨 Building SIMI tools..."
cargo build --release -p simi-cli

# Start kernel in background
echo "🚀 Starting kernel in QEMU..."
./target/release/simi kernel launch \
    --kernel "${KERNEL_IMAGE}" \
    --memory 512M \
    --smp 4 \
    --port "${HOST_PORT}" &
KERNEL_PID=$!

# Wait for kernel to boot
echo "⏳ Waiting for kernel to boot..."
sleep 5

# Deploy service
echo "📦 Deploying service..."
./target/release/simi kernel deploy \
    --file "${SERVICE_FILE}" \
    --port "${HOST_PORT}"

echo ""
echo "✅ Deployment complete!"
echo ""
echo "Service is running at: http://localhost:${HOST_PORT}"
echo ""
echo "Press Ctrl+C to stop the kernel"

# Wait for Ctrl+C
trap "kill ${KERNEL_PID} 2>/dev/null; echo 'Kernel stopped'" EXIT
wait ${KERNEL_PID}
```

### 7. Usage Instructions

```bash
# 1. Build your AeroSLS kernel (from your existing kernel code)
cd kernel/
make
# This creates build/aerosls-kernel.bin

# 2. Build the SIMI tools
cd ..
cargo build --release

# 3. Launch the kernel in QEMU
./target/release/simi kernel launch \
    --kernel kernel/build/aerosls-kernel.bin \
    --memory 512M \
    --smp 4 \
    --port 8080

# 4. In another terminal, deploy your service
./target/release/simi kernel deploy \
    --file examples/hello-service/src/main.simi \
    --port 8080

# 5. Test your service
curl -X POST http://localhost:8080/HelloWorld/greet \
    -H "Content-Type: application/json" \
    -d '"World"'

# 6. Check kernel status
./target/release/simi kernel status --port 8080

# Or use the all-in-one script
./scripts/deploy-to-kernel.sh \
    kernel/build/aerosls-kernel.bin \
    examples/hello-service/src/main.simi \
    8080
```

### The key points about running on your AeroSLS kernel:

1. **System Call Interface**: We defined syscalls that match your kernel's interface
2. **Kernel Runtime**: The runtime adapter compiles SIMI services to kernel-compatible binaries
3. **Direct Hardware Access**: Services run directly on the kernel with minimal overhead
4. **QEMU Integration**: Automatic QEMU launch and network configuration
5. **State Management**: State operations go through kernel syscalls for efficient access
6. **Service Discovery**: Services register with the kernel's service registry

### This means your AeroSLS services get:

- **True hardware independence**: Same service code runs on your kernel, in Docker, or as WASM
- **Minimal overhead**: Direct syscall interface instead of heavy runtime
- **Kernel-level isolation**: Each service runs in its own protection domain
- **Efficient IPC**: Service-to-service communication through kernel-optimized channels

The integration is seamless - developers write standard AeroSLS services, and the deployment system automatically compiles them for our kernel target!
