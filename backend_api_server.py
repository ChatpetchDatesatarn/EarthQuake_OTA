"""
Earthquake Sensor Network - Backend API Server
Version: 2.2.0 - FULL AUTO OTA
Features:
  - Auto-detect outdated nodes from ota_check
  - Auto-download firmware from GitHub
  - Auto-trigger OTA updates
  - SHA256 verification
  - Real-time progress tracking
"""
import os, tempfile, hashlib, requests, json, threading, time
from flask import Flask, jsonify, request, send_from_directory
from flask_cors import CORS
from flask_socketio import SocketIO, emit
from flask_jwt_extended import JWTManager, create_access_token, jwt_required, get_jwt_identity
from werkzeug.security import generate_password_hash, check_password_hash
from werkzeug.utils import secure_filename
import serial
from datetime import datetime, timedelta
import base64

app = Flask(__name__)
app.config['SECRET_KEY'] = 'your-secret-key-change-this-in-production'
app.config['JWT_SECRET_KEY'] = 'jwt-secret-key-change-this-in-production'
app.config['JWT_ACCESS_TOKEN_EXPIRES'] = timedelta(hours=24)
app.config['UPLOAD_FOLDER'] = 'firmware_uploads'
app.config['MAX_CONTENT_LENGTH'] = 16 * 1024 * 1024

CORS(app)
socketio = SocketIO(app, cors_allowed_origins="*")
jwt = JWTManager(app)

os.makedirs(app.config['UPLOAD_FOLDER'], exist_ok=True)
os.makedirs('logs', exist_ok=True)

users = {
    'admin': generate_password_hash('admin123')
}

nodes = []
ota_history = []
gateway_connection = None
serial_thread = None

RS232_PORT = os.getenv('RS232_PORT', 'COM5')
MANIFEST_URL = "https://raw.githubusercontent.com/ChatpetchDatesatarn/EarthQuake_OTA/main/ota/manifest.json"

# ===========================
# AUTO OTA Configuration
# ===========================
AUTO_OTA_ENABLED = True
AUTO_OTA_COOLDOWN = 300  # 5 minutes between auto-updates per node
last_auto_ota = {}  # node_id -> timestamp
manifest_cache = None
manifest_last_fetch = 0

# ===========================
# Manifest Management
# ===========================
def fetch_manifest():
    """Fetch and cache manifest.json from GitHub"""
    global manifest_cache, manifest_last_fetch
    
    # Cache for 5 minutes
    if manifest_cache and (time.time() - manifest_last_fetch) < 300:
        return manifest_cache
    
    try:
        print("📥 Fetching manifest.json from GitHub...")
        response = requests.get(MANIFEST_URL, timeout=10)
        response.raise_for_status()
        manifest_cache = response.json()
        manifest_last_fetch = time.time()
        
        print(f"✅ Manifest loaded: v{manifest_cache.get('version')}")
        return manifest_cache
    except Exception as e:
        print(f"❌ Manifest fetch failed: {e}")
        return None

def get_firmware_url_for_role(role):
    """Get firmware URL from manifest based on ROLE"""
    manifest = fetch_manifest()
    if not manifest:
        return None, None, None
    
    # Try role-based lookup first
    firmware_url = manifest.get('assets', {}).get(role)
    sha256_hash = manifest.get('sha256', {}).get(role)
    version = manifest.get('version')
    
    if not firmware_url:
        print(f"⚠️ No firmware found for role: {role}")
        return None, None, None
    
    return firmware_url, sha256_hash, version

def compare_versions(current, latest):
    """Compare semantic versions (e.g., '2.1.0' vs '2.1.1')"""
    try:
        current_parts = [int(x) for x in current.split('.')]
        latest_parts = [int(x) for x in latest.split('.')]
        
        for c, l in zip(current_parts, latest_parts):
            if l > c:
                return True  # Latest is newer
            elif l < c:
                return False  # Current is newer
        
        return False  # Same version
    except:
        return current != latest  # Fallback to string comparison

# ===========================
# Serial Communication + OTA
# ===========================
class GatewayConnection:
    def __init__(self, port='COM3', baudrate=115200):
        self.port = port
        self.baudrate = baudrate
        self.ser = None
        self.running = False
        self.connected = False
        self.ota_sessions = {}
        
    def connect(self):
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=1)
            self.connected = True
            self.running = True
            print(f"✅ Connected to Gateway on {self.port}")
            return True
        except Exception as e:
            print(f"❌ Failed to connect: {e}")
            self.connected = False
            return False
    
    def disconnect(self):
        self.running = False
        if self.ser and self.ser.is_open:
            self.ser.close()
        self.connected = False
    
    def send_command(self, command):
        if not self.connected or not self.ser:
            print(f"⚠️ Gateway not connected")
            return False
        try:
            data = json.dumps(command) + '\n'
            self.ser.write(data.encode())
            self.ser.flush()
            print(f"📤 Sent: {command.get('type', 'unknown')}")
            return True
        except Exception as e:
            print(f"❌ Send error: {e}")
            return False
    
    def read_loop(self):
        buffer = ""
        while self.running:
            try:
                if self.ser and self.ser.in_waiting:
                    data = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                    buffer += data
                    
                    while '\n' in buffer:
                        line, buffer = buffer.split('\n', 1)
                        line = line.strip()
                        if line:
                            self.process_gateway_message(line)
                time.sleep(0.01)
            except Exception as e:
                print(f"❌ Read error: {e}")
                time.sleep(1)
    
    def process_gateway_message(self, message):
        try:
            data = json.loads(message)
            msg_type = data.get('type', '')
            
            print(f"📥 Received: {msg_type}")
            
            # Route messages
            if msg_type == 'mesh_status':
                self.update_nodes_from_gateway(data)
            elif msg_type == 'node_connected':
                self.handle_node_connected(data)
            elif msg_type == 'node_disconnected':
                self.handle_node_disconnected(data)
            elif msg_type == 'mesh_data':
                self.handle_sensor_data(data)
            elif msg_type == 'ota_check_forward':
                # 🚀 AUTO OTA TRIGGER!
                self.handle_ota_check_auto(data)
            elif msg_type == 'ota_accept':
                self.handle_ota_accept(data)
            elif msg_type == 'ota_next':
                self.handle_ota_next(data)
            elif msg_type == 'ota_result':
                self.handle_ota_result(data)
            elif msg_type == 'ota_error':
                self.handle_ota_error(data)
                
            socketio.emit('gateway_message', data)
            
        except json.JSONDecodeError:
            print(f"⚠️ Invalid JSON: {message[:100]}")
    
    # ===========================
    # 🚀 AUTO OTA HANDLER
    # ===========================
    def handle_ota_check_auto(self, data):
        """
        Automatically trigger OTA when node sends ota_check
        """
        if not AUTO_OTA_ENABLED:
            print("⚠️ Auto OTA is disabled")
            return
        
        node_id = data.get('source_node')
        node_role = data.get('role', '')
        current_fw = data.get('fw_version', '0.0.0')
        
        print(f"\n{'='*60}")
        print(f"🔍 OTA Check from Node {node_id}")
        print(f"   Role: {node_role}")
        print(f"   Current FW: {current_fw}")
        
        # Check cooldown
        last_update = last_auto_ota.get(node_id, 0)
        if time.time() - last_update < AUTO_OTA_COOLDOWN:
            remaining = AUTO_OTA_COOLDOWN - (time.time() - last_update)
            print(f"⏳ Cooldown active: {int(remaining)}s remaining")
            print(f"{'='*60}\n")
            return
        
        # Get firmware info from manifest
        firmware_url, sha256_hash, latest_version = get_firmware_url_for_role(node_role)
        
        if not firmware_url:
            print(f"❌ No firmware URL found for role: {node_role}")
            print(f"{'='*60}\n")
            return
        
        print(f"   Latest FW: {latest_version}")
        print(f"   Firmware URL: {firmware_url}")
        
        # Compare versions
        needs_update = compare_versions(current_fw, latest_version)
        
        if not needs_update:
            print(f"✅ Node is up-to-date!")
            print(f"{'='*60}\n")
            return
        
        print(f"🚀 UPDATE REQUIRED: {current_fw} → {latest_version}")
        print(f"{'='*60}\n")
        
        # Update node info
        node = next((n for n in nodes if n['id'] == node_id), None)
        if not node:
            node = {
                'id': node_id,
                'name': f'Node_{node_id}',
                'token': '',
                'version': current_fw,
                'role': node_role,
                'type': 'ESP32-C3',
                'status': 'online',
                'lastSeen': datetime.now().isoformat(),
                'rssi': 0,
                'temperature': 0
            }
            nodes.append(node)
        
        # Trigger auto OTA in background thread
        threading.Thread(
            target=self.auto_ota_worker,
            args=(node_id, node.get('name', f'Node_{node_id}'), firmware_url, sha256_hash, latest_version),
            daemon=True
        ).start()
        
        last_auto_ota[node_id] = time.time()
    
    def auto_ota_worker(self, node_id, node_name, firmware_url, sha256_hash, version):
        """Background worker for auto OTA"""
        print(f"\n🤖 [AUTO OTA] Starting for {node_name} (Node {node_id})")
        print(f"   Version: {version}")
        print(f"   URL: {firmware_url}")
        
        socketio.emit('auto_ota_started', {
            'node_id': node_id,
            'node_name': node_name,
            'version': version,
            'timestamp': datetime.now().isoformat()
        })
        
        # Download firmware
        try:
            print(f"📥 [AUTO OTA] Downloading firmware...")
            with requests.get(firmware_url, stream=True, timeout=30) as r:
                r.raise_for_status()
                fd, temp_path = tempfile.mkstemp(prefix="auto_ota_", suffix=".bin")
                os.close(fd)
                
                total_size = int(r.headers.get('content-length', 0))
                downloaded = 0
                
                with open(temp_path, "wb") as f:
                    for chunk in r.iter_content(chunk_size=128*1024):
                        if chunk:
                            f.write(chunk)
                            downloaded += len(chunk)
                            if total_size > 0:
                                progress = (downloaded / total_size) * 100
                                print(f"📥 Download: {progress:.1f}% ({downloaded}/{total_size})", end='\r')
                
                print(f"\n✅ [AUTO OTA] Download complete: {downloaded} bytes")
        except Exception as e:
            print(f"❌ [AUTO OTA] Download failed: {e}")
            socketio.emit('auto_ota_failed', {
                'node_id': node_id,
                'error': f'Download failed: {e}',
                'timestamp': datetime.now().isoformat()
            })
            return
        
        # Verify SHA256
        if sha256_hash:
            try:
                print(f"🔐 [AUTO OTA] Verifying SHA256...")
                h = hashlib.sha256()
                with open(temp_path, "rb") as f:
                    for chunk in iter(lambda: f.read(1024*1024), b""):
                        h.update(chunk)
                got = h.hexdigest().lower()
                expected = sha256_hash.lower()
                
                if got != expected:
                    print(f"❌ [AUTO OTA] SHA256 mismatch!")
                    print(f"   Expected: {expected}")
                    print(f"   Got:      {got}")
                    os.remove(temp_path)
                    socketio.emit('auto_ota_failed', {
                        'node_id': node_id,
                        'error': 'SHA256 verification failed',
                        'timestamp': datetime.now().isoformat()
                    })
                    return
                
                print(f"✅ [AUTO OTA] SHA256 verified!")
            except Exception as e:
                print(f"❌ [AUTO OTA] SHA256 verification error: {e}")
                os.remove(temp_path)
                return
        
        # Send OTA
        try:
            with open(temp_path, 'rb') as f:
                firmware_data = f.read()
            
            file_size = len(firmware_data)
            print(f"📦 [AUTO OTA] Firmware ready: {file_size/1024:.1f} KB")
            
            # Create OTA session
            self.ota_sessions[node_id] = {
                'firmware_data': firmware_data,
                'version': version,
                'size': file_size,
                'chunk_size': 1024,
                'started_at': datetime.now().isoformat(),
                'node_name': node_name,
                'auto': True
            }
            
            # Send OTA offer
            ota_offer = {
                'type': 'ota_offer',
                'target_node': node_id,
                'version': version,
                'size': file_size,
                'chunk': 1024
            }
            
            success = self.send_command(ota_offer)
            
            if success:
                print(f"✅ [AUTO OTA] OTA offer sent to node {node_id}")
                
                # Log to history
                ota_event = {
                    'id': len(ota_history) + 1,
                    'node_id': node_id,
                    'node_name': node_name,
                    'version': version,
                    'status': 'initiated',
                    'initiated_by': 'AUTO_OTA',
                    'timestamp': datetime.now().isoformat(),
                    'file_size': file_size
                }
                ota_history.append(ota_event)
                log_event('auto_ota', f'Auto OTA initiated for {node_name} v{version}')
                
                # Update node status
                node = next((n for n in nodes if n['id'] == node_id), None)
                if node:
                    node['status'] = 'updating'
                
                socketio.emit('node_status_change', {
                    'node_id': node_id,
                    'status': 'updating'
                })
            else:
                print(f"❌ [AUTO OTA] Failed to send OTA offer")
                del self.ota_sessions[node_id]
                
        except Exception as e:
            print(f"❌ [AUTO OTA] Error: {e}")
            socketio.emit('auto_ota_failed', {
                'node_id': node_id,
                'error': str(e),
                'timestamp': datetime.now().isoformat()
            })
        finally:
            try:
                os.remove(temp_path)
            except:
                pass
    
    # ===========================
    # OTA Protocol Handlers
    # ===========================
    def handle_ota_accept(self, data):
        node_id = data.get('source_node')
        device_name = data.get('device_name', f'Node_{node_id}')
        
        print(f"✅ [OTA] Node {node_id} ({device_name}) accepted")
        
        if node_id not in self.ota_sessions:
            print(f"⚠️ [OTA] No session for node {node_id}")
            return
        
        is_auto = self.ota_sessions[node_id].get('auto', False)
        if is_auto:
            print(f"🤖 [AUTO OTA] Node accepted, starting transfer...")
        
        self.send_ota_chunk(node_id, 0)
    
    def handle_ota_next(self, data):
        node_id = data.get('source_node')
        next_idx = data.get('idx', 0)
        
        if node_id not in self.ota_sessions:
            print(f"⚠️ [OTA] No session for node {node_id}")
            return
        
        self.send_ota_chunk(node_id, next_idx)
    
    def handle_ota_result(self, data):
        node_id = data.get('source_node')
        success = data.get('ok', False)
        msg_text = data.get('msg', '')
        new_version = data.get('new_version', '')
        
        is_auto = self.ota_sessions.get(node_id, {}).get('auto', False)
        prefix = "🤖 [AUTO OTA]" if is_auto else "[OTA]"
        
        print(f"{'✅' if success else '❌'} {prefix} Node {node_id}: {msg_text}")
        
        if success and new_version:
            node = next((n for n in nodes if n['id'] == node_id), None)
            if node:
                old_version = node.get('version', 'unknown')
                node['version'] = new_version
                node['status'] = 'online'
                print(f"✅ {prefix} Updated {old_version} → {new_version}")
                
                for ota_event in ota_history:
                    if ota_event.get('node_id') == node_id and ota_event.get('status') == 'initiated':
                        ota_event['status'] = 'completed' if success else 'failed'
                        ota_event['completed_at'] = datetime.now().isoformat()
                        break
        
        if node_id in self.ota_sessions:
            del self.ota_sessions[node_id]
            print(f"🗑️ {prefix} Session cleaned up")
        
        event_name = 'auto_ota_complete' if is_auto else 'ota_complete'
        socketio.emit(event_name, {
            'node_id': node_id,
            'success': success,
            'message': msg_text,
            'new_version': new_version,
            'timestamp': datetime.now().isoformat()
        })
    
    def handle_ota_error(self, data):
        node_id = data.get('source_node')
        error_msg = data.get('message', 'Unknown error')
        
        is_auto = self.ota_sessions.get(node_id, {}).get('auto', False)
        prefix = "🤖 [AUTO OTA]" if is_auto else "[OTA]"
        
        print(f"❌ {prefix} Node {node_id} error: {error_msg}")
        
        if node_id in self.ota_sessions:
            del self.ota_sessions[node_id]
        
        event_name = 'auto_ota_error' if is_auto else 'ota_error'
        socketio.emit(event_name, {
            'node_id': node_id,
            'error': error_msg,
            'timestamp': datetime.now().isoformat()
        })
    
    def send_ota_chunk(self, node_id, chunk_idx):
        if node_id not in self.ota_sessions:
            print(f"⚠️ [OTA] No session")
            return False
        
        session = self.ota_sessions[node_id]
        firmware = session['firmware_data']
        chunk_size = session.get('chunk_size', 1024)
        total_size = len(firmware)
        is_auto = session.get('auto', False)
        
        start = chunk_idx * chunk_size
        end = min(start + chunk_size, total_size)
        
        if start >= total_size:
            end_msg = {
                'type': 'ota_end',
                'target_node': node_id
            }
            self.send_command(end_msg)
            prefix = "🤖 [AUTO OTA]" if is_auto else "[OTA]"
            print(f"🏁 {prefix} Transfer complete for node {node_id}")
            return True
        
        chunk_data = firmware[start:end]
        chunk_b64 = base64.b64encode(chunk_data).decode('ascii')
        
        chunk_msg = {
            'type': 'ota_chunk',
            'target_node': node_id,
            'idx': chunk_idx,
            'data': chunk_b64
        }
        
        success = self.send_command(chunk_msg)
        
        if success:
            bytes_sent = min(end, total_size)
            progress = (bytes_sent / total_size) * 100
            
            event_name = 'auto_ota_progress' if is_auto else 'ota_progress'
            socketio.emit(event_name, {
                'node_id': node_id,
                'progress': int(progress),
                'chunk': chunk_idx,
                'bytes_sent': bytes_sent,
                'total_bytes': total_size
            })
        
        return success
    
    def update_nodes_from_gateway(self, data):
        active_nodes = data.get('active_nodes', [])
        for node_data in active_nodes:
            node_id = node_data.get('node_id')
            existing = next((n for n in nodes if n['id'] == node_id), None)
            
            if existing:
                existing['status'] = 'online' if node_data.get('is_active') else 'offline'
                existing['version'] = node_data.get('fw_version', 'unknown')
                existing['lastSeen'] = datetime.now().isoformat()
                existing['rssi'] = node_data.get('signal_strength', 0)
            else:
                new_node = {
                    'id': node_id,
                    'name': node_data.get('device_name', f'Node_{node_id}'),
                    'token': node_data.get('access_token', ''),
                    'version': node_data.get('fw_version', 'unknown'),
                    'type': 'ESP32-C3',
                    'status': 'online' if node_data.get('is_active') else 'offline',
                    'lastSeen': datetime.now().isoformat(),
                    'rssi': node_data.get('signal_strength', 0),
                    'temperature': 0
                }
                nodes.append(new_node)
                socketio.emit('node_added', new_node)
    
    def handle_node_connected(self, data):
        node_id = data.get('node_id')
        print(f"🔗 Node {node_id} connected")
        socketio.emit('node_status_change', {
            'node_id': node_id,
            'status': 'online',
            'timestamp': datetime.now().isoformat()
        })
    
    def handle_node_disconnected(self, data):
        node_id = data.get('node_id')
        print(f"🔌 Node {node_id} disconnected")
        socketio.emit('node_status_change', {
            'node_id': node_id,
            'status': 'offline',
            'timestamp': datetime.now().isoformat()
        })
    
    def handle_sensor_data(self, data):
        node_id = data.get('source_node')
        sensor_data = data.get('data', {})
        
        node = next((n for n in nodes if n['id'] == node_id), None)
        if node and 'earthquake' in sensor_data:
            eq_data = sensor_data['earthquake']
            node['temperature'] = eq_data.get('temp', 0)
            socketio.emit('sensor_update', {
                'node_id': node_id,
                'data': sensor_data,
                'timestamp': datetime.now().isoformat()
            })

# ===========================
# Manual OTA (still available)
# ===========================
def ota_update_core(node_id: int, version: str, filepath: str, initiated_by: str):
    if not os.path.exists(filepath):
        return jsonify({'error': 'Firmware not found'}), 404

    try:
        node_id = int(node_id)
    except:
        return jsonify({'error': 'Invalid node_id'}), 400

    node = next((n for n in nodes if n['id'] == node_id), None)
    if not node:
        return jsonify({'error': 'Node not found'}), 404

    if not (gateway_connection and gateway_connection.connected):
        return jsonify({'error': 'Gateway not connected'}), 503

    with open(filepath, 'rb') as f:
        firmware_data = f.read()
    
    file_size = len(firmware_data)
    
    print(f"📦 [MANUAL OTA] Starting for {node['name']}")
    print(f"    Version: {version}, Size: {file_size/1024:.1f} KB")

    gateway_connection.ota_sessions[node_id] = {
        'firmware_data': firmware_data,
        'version': version,
        'size': file_size,
        'chunk_size': 1024,
        'started_at': datetime.now().isoformat(),
        'node_name': node['name'],
        'auto': False
    }

    ota_offer = {
        'type': 'ota_offer',
        'target_node': node_id,
        'version': version,
        'size': file_size,
        'chunk': 1024
    }
    
    success = gateway_connection.send_command(ota_offer)
    
    if not success:
        del gateway_connection.ota_sessions[node_id]
        return jsonify({'error': 'Failed to send OTA offer'}), 500

    ota_event = {
        'id': len(ota_history) + 1,
        'node_id': node_id,
        'node_name': node['name'],
        'version': version,
        'status': 'initiated',
        'initiated_by': initiated_by,
        'timestamp': datetime.now().isoformat(),
        'file_size': file_size
    }
    ota_history.append(ota_event)
    log_event('ota', f'Manual OTA for {node["name"]} v{version} by {initiated_by}')

    node['status'] = 'updating'
    socketio.emit('node_status_change', {
        'node_id': node_id,
        'status': 'updating'
    })

    return jsonify({
        'success': True,
        'ota_id': ota_event['id'],
        'message': f'OTA initiated for {node["name"]} to v{version}'
    })

# ===========================
# API Endpoints
# ===========================
@app.route('/api/auth/login', methods=['POST'])
def login():
    username = request.json.get('username')
    password = request.json.get('password')
    
    if username not in users or not check_password_hash(users[username], password):
        return jsonify({'error': 'Invalid credentials'}), 401
    
    access_token = create_access_token(identity=username)
    log_event('auth', f'User {username} logged in')
    
    return jsonify({
        'access_token': access_token,
        'username': username
    })

@app.route('/api/nodes', methods=['GET'])
@jwt_required()
def get_nodes():
    return jsonify(nodes)

@app.route('/api/nodes', methods=['POST'])
@jwt_required()
def add_node():
    data = request.json
    current_user = get_jwt_identity()
    
    new_node = {
        'id': int(time.time() * 1000),
        'name': data.get('name'),
        'token': data.get('token'),
        'type': data.get('type', 'ESP32-C3'),
        'version': 'pending',
        'status': 'offline',
        'lastSeen': None,
        'rssi': 0,
        'temperature': 0
    }
    
    nodes.append(new_node)
    log_event('node', f'Node {new_node["name"]} added by {current_user}')
    socketio.emit('node_added', new_node)
    
    return jsonify({'success': True, 'node': new_node})

@app.route('/api/ota/upload', methods=['POST'])
@jwt_required()
def upload_firmware():
    current_user = get_jwt_identity()
    
    if 'file' not in request.files:
        return jsonify({'error': 'No file provided'}), 400
    
    file = request.files['file']
    version = request.form.get('version', '2.2.0')
    
    if not file.filename.endswith('.bin'):
        return jsonify({'error': 'Only .bin files allowed'}), 400
    
    filename = secure_filename(file.filename)
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    filename = f"{timestamp}_{version}_{filename}"
    filepath = os.path.join(app.config['UPLOAD_FOLDER'], filename)
    
    file.save(filepath)
    file_size = os.path.getsize(filepath)
    
    log_event('ota', f'Firmware {filename} uploaded ({file_size} bytes)')
    
    return jsonify({
        'success': True,
        'filename': filename,
        'size': file_size,
        'version': version
    })

@app.route('/api/ota/update', methods=['POST'])
@jwt_required()
def initiate_ota():
    current_user = get_jwt_identity()
    data = request.json or {}
    
    node_id = data.get('node_id')
    firmware_file = data.get('firmware_file')
    version = data.get('version')
    
    if not all([node_id, firmware_file, version]):
        return jsonify({'error': 'Missing required fields'}), 400
    
    filepath = os.path.join(app.config['UPLOAD_FOLDER'], firmware_file)
    return ota_update_core(node_id, str(version).strip(), filepath, current_user)

@app.route('/api/ota/update_from_github', methods=['POST'])
@jwt_required()
def ota_from_github():
    current_user = get_jwt_identity()
    data = request.get_json(force=True) or {}
    
    node_id = data.get('node_id')
    version = str(data.get('version', '')).strip()
    firmware_url = str(data.get('firmware_url', '')).strip()
    expected_sha256 = str(data.get('sha256', '')).lower().strip() if data.get('sha256') else ''
    
    if not all([node_id, version, firmware_url]):
        return jsonify({'error': 'Missing required fields'}), 400
    
    try:
        with requests.get(firmware_url, stream=True, timeout=30) as r:
            r.raise_for_status()
            fd, temp_path = tempfile.mkstemp(prefix="ota_", suffix=".bin")
            os.close(fd)
            with open(temp_path, "wb") as f:
                for chunk in r.iter_content(chunk_size=128*1024):
                    if chunk:
                        f.write(chunk)
    except Exception as e:
        return jsonify({'error': f'Download failed: {e}'}), 502
    
    if expected_sha256:
        try:
            h = hashlib.sha256()
            with open(temp_path, "rb") as f:
                for chunk in iter(lambda: f.read(1024*1024), b""):
                    h.update(chunk)
            got = h.hexdigest().lower()
            if got != expected_sha256:
                os.remove(temp_path)
                return jsonify({'error': 'SHA256 mismatch'}), 400
        except Exception as e:
            os.remove(temp_path)
            return jsonify({'error': f'SHA256 error: {e}'}), 500
    
    try:
        resp = ota_update_core(node_id, version, temp_path, current_user)
    finally:
        try:
            os.remove(temp_path)
        except:
            pass
    
    return resp

@app.route('/api/ota/history', methods=['GET'])
@jwt_required()
def get_ota_history():
    return jsonify(ota_history)

# ===========================
# Auto OTA Configuration
# ===========================
@app.route('/api/ota/auto/status', methods=['GET'])
@jwt_required()
def get_auto_ota_status():
    manifest = fetch_manifest()
    return jsonify({
        'enabled': AUTO_OTA_ENABLED,
        'cooldown': AUTO_OTA_COOLDOWN,
        'manifest_version': manifest.get('version') if manifest else None,
        'manifest_cached': manifest is not None,
        'last_auto_updates': {str(k): v for k, v in last_auto_ota.items()}
    })

@app.route('/api/ota/auto/toggle', methods=['POST'])
@jwt_required()
def toggle_auto_ota():
    global AUTO_OTA_ENABLED
    AUTO_OTA_ENABLED = not AUTO_OTA_ENABLED
    
    status = "enabled" if AUTO_OTA_ENABLED else "disabled"
    log_event('auto_ota', f'Auto OTA {status}')
    
    return jsonify({
        'success': True,
        'enabled': AUTO_OTA_ENABLED
    })

@app.route('/api/ota/manifest', methods=['GET'])
def get_manifest():
    """Get current manifest.json"""
    manifest = fetch_manifest()
    if manifest:
        return jsonify(manifest)
    else:
        return jsonify({'error': 'Failed to fetch manifest'}), 500

@app.route('/api/ota/manifest/refresh', methods=['POST'])
@jwt_required()
def refresh_manifest():
    """Force refresh manifest cache"""
    global manifest_cache, manifest_last_fetch
    manifest_cache = None
    manifest_last_fetch = 0
    
    manifest = fetch_manifest()
    if manifest:
        return jsonify({
            'success': True,
            'version': manifest.get('version'),
            'timestamp': datetime.now().isoformat()
        })
    else:
        return jsonify({'error': 'Failed to refresh manifest'}), 500

# ===========================
# Gateway Management
# ===========================
@app.route('/api/gateway/status', methods=['GET'])
@jwt_required()
def gateway_status():
    return jsonify({
        'connected': gateway_connection.connected if gateway_connection else False,
        'port': gateway_connection.port if gateway_connection else None,
        'active_ota_sessions': len(gateway_connection.ota_sessions) if gateway_connection else 0,
        'auto_ota_enabled': AUTO_OTA_ENABLED
    })

@app.route('/api/gateway/connect', methods=['POST'])
@jwt_required()
def connect_gateway():
    global gateway_connection, serial_thread
    
    port = request.json.get('port', RS232_PORT)
    baudrate = request.json.get('baudrate', 115200)
    
    if gateway_connection and gateway_connection.connected:
        return jsonify({'error': 'Already connected'}), 400
    
    gateway_connection = GatewayConnection(port, baudrate)
    if gateway_connection.connect():
        serial_thread = threading.Thread(target=gateway_connection.read_loop, daemon=True)
        serial_thread.start()
        log_event('gateway', f'Connected on {port}')
        return jsonify({'success': True})
    else:
        return jsonify({'error': 'Connection failed'}), 500

@app.route('/api/gateway/disconnect', methods=['POST'])
@jwt_required()
def disconnect_gateway():
    if gateway_connection:
        gateway_connection.disconnect()
        log_event('gateway', 'Disconnected')
    return jsonify({'success': True})

# ===========================
# Statistics
# ===========================
@app.route('/api/stats', methods=['GET'])
def get_stats():
    manifest = fetch_manifest()
    latest_version = manifest.get('version', '2.2.0') if manifest else '2.2.0'
    
    total = len(nodes)
    online = len([n for n in nodes if n['status'] == 'online'])
    outdated = len([n for n in nodes if n.get('version', '0.0.0') != latest_version])
    updating = len([n for n in nodes if n['status'] == 'updating'])
    
    return jsonify({
        'total_nodes': total,
        'online_nodes': online,
        'outdated_nodes': outdated,
        'updating_nodes': updating,
        'latest_version': latest_version,
        'gateway_connected': gateway_connection.connected if gateway_connection else False,
        'active_ota_sessions': len(gateway_connection.ota_sessions) if gateway_connection else 0,
        'auto_ota_enabled': AUTO_OTA_ENABLED
    })

def log_event(event_type, message):
    timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    log_message = f"[{timestamp}] {message}\n"
    
    log_file = f'logs/{event_type}.log'
    with open(log_file, 'a') as f:
        f.write(log_message)
    
    with open('logs/all.log', 'a') as f:
        f.write(f"[{event_type.upper()}] {log_message}")
    
    print(log_message.strip())

# ===========================
# WebSocket
# ===========================
@socketio.on('connect')
def handle_connect():
    print('Client connected')
    emit('connection_response', {'status': 'connected'})

@socketio.on('disconnect')
def handle_disconnect():
    print('Client disconnected')

@socketio.on('request_auto_ota_status')
def handle_auto_ota_status_request():
    emit('auto_ota_status', {
        'enabled': AUTO_OTA_ENABLED,
        'active_sessions': len(gateway_connection.ota_sessions) if gateway_connection else 0
    })

# ===========================
# Static Files
# ===========================
@app.route('/')
def serve_index():
    return send_from_directory('static', 'index.html')

@app.route('/<path:path>')
def serve_static(path):
    return send_from_directory('static', path)

# ===========================
# Startup
# ===========================
if __name__ == '__main__':
    print("="*60)
    print("Earthquake Sensor Network - Backend v2.2.0")
    print("🚀 FULL AUTO OTA ENABLED")
    print("="*60)
    print("\nNew Features:")
    print("  ✅ Auto-detect outdated nodes")
    print("  ✅ Auto-download from GitHub")
    print("  ✅ Auto-trigger OTA updates")
    print("  ✅ SHA256 verification")
    print("  ✅ Real-time progress tracking")
    print("\nDefault Credentials:")
    print("  Username: admin")
    print("  Password: admin123")
    print("\nEndpoints:")
    print("  Web UI:  http://localhost:5000")
    print("  API:     http://localhost:5000/api")
    print("\nAuto OTA Configuration:")
    print(f"  Enabled:  {AUTO_OTA_ENABLED}")
    print(f"  Cooldown: {AUTO_OTA_COOLDOWN}s (5 minutes)")
    print(f"  Manifest: {MANIFEST_URL}")
    print("="*60)
    
    # Pre-fetch manifest
    print("\n📥 Pre-fetching manifest...")
    manifest = fetch_manifest()
    if manifest:
        print(f"✅ Manifest ready: v{manifest.get('version')}")
        print(f"   Available roles: {len(manifest.get('assets', {}))} firmwares")
    else:
        print("⚠️ Manifest fetch failed - will retry on first OTA check")
    
    print(f"\n🔌 Connecting to Gateway on {RS232_PORT}...")
    gateway_connection = GatewayConnection(RS232_PORT, 115200)
    
    if gateway_connection.connect():
        serial_thread = threading.Thread(target=gateway_connection.read_loop, daemon=True)
        serial_thread.start()
        print(f"✅ Gateway connected on {RS232_PORT}")
        print(f"🤖 Auto OTA monitoring active!")
    else:
        print(f"⚠️ Gateway connection failed")
        print(f"   Connect manually via POST /api/gateway/connect")
    
    print("\n" + "="*60)
    print("🚀 Server starting...")
    print("   Nodes will auto-update when they send ota_check!")
    print("="*60 + "\n")
    
    socketio.run(app, host='0.0.0.0', port=5000, debug=True)