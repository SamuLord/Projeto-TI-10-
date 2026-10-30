"""
Backend local: MQTT -> SQLite + API HTTP para relatórios.
Instalação: pip install flask paho-mqtt
Execute: python backend.py
"""
import json, sqlite3, threading
from datetime import datetime, timezone, timedelta
from flask import Flask, jsonify, request, send_from_directory
import paho.mqtt.client as mqtt

DB = "qee_history.sqlite3"
MQTT_HOST = "127.0.0.1"
MQTT_PORT = 1883
DEVICE_TOPIC = "qee/esp32-qee-01/#"
app = Flask(__name__, static_folder="web", static_url_path="")
db_lock = threading.Lock()

def connect_db():
    con = sqlite3.connect(DB, check_same_thread=False)
    con.execute("""CREATE TABLE IF NOT EXISTS events (
        id INTEGER PRIMARY KEY AUTOINCREMENT, ts TEXT NOT NULL,
        event TEXT, relay TEXT, reason TEXT, duration_ms INTEGER,
        payload TEXT)""")
    con.execute("""CREATE TABLE IF NOT EXISTS status (
        id INTEGER PRIMARY KEY AUTOINCREMENT, ts TEXT NOT NULL, payload TEXT NOT NULL)""")
    con.execute("""CREATE TABLE IF NOT EXISTS wave (
        id INTEGER PRIMARY KEY AUTOINCREMENT, ts TEXT NOT NULL,
        samples TEXT NOT NULL, sample_rate_hz REAL)""")
    con.commit()
    return con

db = connect_db()

def now_iso():
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")

def on_connect(client, userdata, flags, reason_code, properties=None):
    client.subscribe(DEVICE_TOPIC)

def on_message(client, userdata, msg):
    ts = now_iso()
    try:
        data = json.loads(msg.payload.decode("utf-8"))
    except Exception:
        return
    with db_lock:
        if msg.topic.endswith("/events"):
            db.execute("INSERT INTO events(ts,event,relay,reason,duration_ms,payload) VALUES(?,?,?,?,?,?)",
                       (ts, data.get("event"), data.get("relay"), data.get("reason"),
                        data.get("duration_ms", 0), json.dumps(data, ensure_ascii=False)))
        elif msg.topic.endswith("/status"):
            db.execute("INSERT INTO status(ts,payload) VALUES(?,?)", (ts, json.dumps(data)))
        elif msg.topic.endswith("/wave"):
            db.execute("INSERT INTO wave(ts,samples,sample_rate_hz) VALUES(?,?,?)",
                       (ts, json.dumps(data.get("samples", [])), data.get("sample_rate_hz", 0)))
        db.commit()

mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="qee-backend")
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message
mqtt_client.connect_async(MQTT_HOST, MQTT_PORT, 30)
mqtt_client.loop_start()

@app.get("/")
def index():
    return send_from_directory("web", "index.html")

@app.get("/api/status")
def get_status():
    with db_lock:
        row = db.execute("SELECT payload,ts FROM status ORDER BY id DESC LIMIT 1").fetchone()
    return jsonify({"connected_data": bool(row), "received_at": row[1] if row else None,
                    "status": json.loads(row[0]) if row else {}})

@app.get("/api/events")
def get_events():
    period = request.args.get("period", "24h")
    days = {"24h": 1, "30d": 30, "1y": 365}.get(period, 1)
    since = (datetime.now().astimezone() - timedelta(days=days)).isoformat(timespec="seconds")
    with db_lock:
        rows = db.execute("SELECT ts,event,relay,reason,duration_ms FROM events WHERE ts >= ? ORDER BY id DESC",
                          (since,)).fetchall()
    return jsonify([{"ts":r[0],"event":r[1],"relay":r[2],"reason":r[3],"duration_ms":r[4]} for r in rows])

@app.get("/api/metrics")
def get_metrics():
    period = request.args.get("period", "24h")
    days = {"24h": 1, "30d": 30, "1y": 365}.get(period, 1)
    since = (datetime.now().astimezone() - timedelta(days=days)).isoformat(timespec="seconds")
    with db_lock:
        rows = db.execute("""SELECT relay, duration_ms FROM events
            WHERE ts >= ? AND event='restore' AND relay IN ('R1','R2','R3')""", (since,)).fetchall()
        trips = db.execute("""SELECT relay, COUNT(*) FROM events
            WHERE ts >= ? AND event='trip' AND relay IN ('R1','R2','R3') GROUP BY relay""", (since,)).fetchall()
    duration_by_relay = {}
    count_by_relay = {}
    for relay, duration in rows:
        duration_by_relay[relay] = duration_by_relay.get(relay, 0) + (duration or 0)
    for relay, count in trips:
        count_by_relay[relay] = count
    # Métricas demonstrativas por relé/ramal. DEC/FEC regulatórios exigem população
    # de unidades consumidoras e metodologia de apuração definida; aqui usa-se N configurável.
    consumers = max(1, int(request.args.get("consumers", "2")))
    result = {}
    for relay in ("R1","R2","R3"):
        result[relay] = {
            "interruptions": count_by_relay.get(relay, 0),
            "interruption_hours_total": duration_by_relay.get(relay, 0) / 3600000,
            "dec_hours_per_consumer_demo": duration_by_relay.get(relay, 0) / 3600000 / consumers,
            "fec_interruptions_per_consumer_demo": count_by_relay.get(relay, 0) / consumers
        }
    return jsonify({"period": period, "consumers_assumed": consumers, "by_relay": result,
                    "note": "Estimativa didática; não equivale à apuração regulatória da distribuidora."})

@app.get("/api/wave")
def get_wave():
    with db_lock:
        row = db.execute("SELECT ts,samples,sample_rate_hz FROM wave ORDER BY id DESC LIMIT 1").fetchone()
    return jsonify({"ts":row[0],"samples":json.loads(row[1]),"sample_rate_hz":row[2]} if row else {})

@app.post("/api/command")
def command():
    data = request.get_json(force=True)
    if not mqtt_client.is_connected():
        return jsonify({"ok":False,"error":"Backend não conectado ao broker MQTT"}), 503
    info = mqtt_client.publish("qee/esp32-qee-01/cmd", json.dumps(data), qos=0)
    return jsonify({"ok": info.rc == mqtt.MQTT_ERR_SUCCESS})

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
