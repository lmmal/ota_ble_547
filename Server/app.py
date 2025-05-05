from flask import Flask, send_from_directory, jsonify
import os
import json

app = Flask(__name__)
firmware_dir = os.path.join(os.path.dirname(__file__), 'firmware')

@app.route('/api/firmware', methods=['GET'])
def latest_firmware():
    """
    Returns the latest firmware version available.
    """
    versions = sorted(os.listdir(firmware_dir), reverse=True)
    for v in versions:
        if v.endswith('.json'):
            with open(os.path.join(firmware_dir, v), 'r') as f:
                data = json.load(f)
                return jsonify(data)
    
    return jsonify({"error": "No firmware found"}), 404

@ app.route('/firmware/<filename>', methods=['GET'])
def download_firmware(filename):
    """
    Download the firmware file.
    """
    try:
        return send_from_directory(firmware_dir, filename, as_attachment=True)
    except FileNotFoundError:
        return jsonify({"error": "File not found"}), 404

if __name__ == "__main__":
    app.run(debug=True, port=5000)
        