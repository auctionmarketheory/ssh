#include <iostream>
#include <string>
#include <vector>
#include <SDL.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "def_wt.h"

// Font inclusion using stb_truetype
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Joystick* g_joystick = nullptr;

// Font system
stbtt_bakedchar cdata[96];
stbtt_bakedchar cdata_mono[96];
SDL_Texture* font_texture = nullptr;
SDL_Texture* font_texture_mono = nullptr;

void initFont(const std::string& path, SDL_Texture** tex, stbtt_bakedchar* cdata_arr) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        std::cerr << "Cannot open font: " << path << std::endl;
        return;
    }
    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    unsigned char* ttf_buffer = new unsigned char[size];
    fread(ttf_buffer, 1, size, file);
    fclose(file);
    
    unsigned char temp_bitmap[512*512];
    stbtt_BakeFontBitmap(ttf_buffer, 0, FONT_SIZE, temp_bitmap, 512, 512, 32, 96, cdata_arr);
    delete[] ttf_buffer;
    
    unsigned char* rgba_bitmap = new unsigned char[512*512*4];
    for (int i = 0; i < 512*512; ++i) {
        rgba_bitmap[i*4 + 0] = 255;
        rgba_bitmap[i*4 + 1] = 255;
        rgba_bitmap[i*4 + 2] = 255;
        rgba_bitmap[i*4 + 3] = temp_bitmap[i];
    }
    
    *tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 512, 512);
    SDL_UpdateTexture(*tex, nullptr, rgba_bitmap, 512 * 4);
    SDL_SetTextureBlendMode(*tex, SDL_BLENDMODE_BLEND);
    delete[] rgba_bitmap;
}

void drawText(SDL_Texture* tex, stbtt_bakedchar* cdata_arr, const std::string& text, float x, float y, SDL_Color color) {
    SDL_SetTextureColorMod(tex, color.r, color.g, color.b);
    for (char c : text) {
        if (c >= 32 && c < 128) {
            stbtt_aligned_quad q;
            stbtt_GetBakedQuad(cdata_arr, 512, 512, c - 32, &x, &y, &q, 1);
            SDL_Rect src = { (int)(q.s0 * 512.0f), (int)(q.t0 * 512.0f), (int)((q.s1 - q.s0) * 512.0f), (int)((q.t1 - q.t0) * 512.0f) };
            SDL_Rect dst = { (int)q.x0, (int)q.y0, (int)(q.x1 - q.x0), (int)(q.y1 - q.y0) };
            SDL_RenderCopy(g_renderer, tex, &src, &dst);
        }
    }
}

// App State
enum AppState { STATE_IDLE, STATE_SERVING, STATE_MENU };
struct App {
    AppState    state      = STATE_IDLE;
    std::string ip         = "";
    pid_t       server_pid = -1;
    FILE*       server_out = nullptr;
    std::vector<std::string> log_lines;
    int         menu_sel   = 0;
    bool        quit       = false;
} app;

// Python backend string
const char* PY_SERVER = R"(
import http.server
import socketserver
import os
import sys
import html
import io
import urllib.parse

PORT = 8000
# Serve from /storage root to allow browsing all folders like 351files
ROOT_DIR = '/storage' if os.path.exists('/storage') else ('/roms' if os.path.exists('/roms') else '/tmp')
os.chdir(ROOT_DIR)

# Tiny 1x1 transparent GIF for favicon
FAVICON = bytes([
    0x47,0x49,0x46,0x38,0x39,0x61,0x01,0x00,0x01,0x00,0x80,0x00,0x00,
    0xff,0x00,0xff,0x00,0x00,0x00,0x21,0xf9,0x04,0x00,0x00,0x00,0x00,
    0x00,0x2c,0x00,0x00,0x00,0x00,0x01,0x00,0x01,0x00,0x00,0x02,0x02,
    0x44,0x01,0x00,0x3b
])

class ROMUploadHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        print(f"[{self.command}] {self.path} {args[1]}", flush=True)

    def do_GET(self):
        # Handle favicon - return tiny GIF
        if self.path == '/favicon.ico':
            self.send_response(200)
            self.send_header('Content-Type', 'image/gif')
            self.send_header('Content-Length', str(len(FAVICON)))
            self.end_headers()
            self.wfile.write(FAVICON)
            return

        f = self.send_head()
        if f:
            try:
                self.copyfile(f, self.wfile)
            finally:
                f.close()

    def do_POST(self):
        try:
            length = int(self.headers.get('Content-Length', 0))
            content_type = self.headers.get('Content-Type', '')
            
            if 'multipart/form-data' not in content_type:
                self.send_response(400)
                self.end_headers()
                return
            
            # Parse boundary
            boundary = ''
            for part in content_type.split(';'):
                part = part.strip()
                if part.startswith('boundary='):
                    boundary = part[9:].strip()
                    break
            
            if not boundary:
                self.send_response(400)
                self.end_headers()
                return
            
            raw_data = self.rfile.read(length)
            
            # Find filename
            filename = ''
            file_data = b''
            boundary_bytes = ('--' + boundary).encode()
            parts = raw_data.split(boundary_bytes)
            for part in parts:
                if b'filename="' in part:
                    header_end = part.find(b'\r\n\r\n')
                    if header_end == -1:
                        continue
                    headers_raw = part[:header_end].decode('utf-8', errors='replace')
                    body = part[header_end+4:]
                    if body.endswith(b'\r\n'):
                        body = body[:-2]
                    # Extract filename
                    for h in headers_raw.split('\r\n'):
                        if 'filename="' in h:
                            start = h.find('filename="') + 10
                            end = h.find('"', start)
                            filename = h[start:end]
                    file_data = body
                    break
            
            if filename and file_data:
                rel_path = urllib.parse.unquote(self.path)
                if rel_path.startswith('/'):
                    rel_path = rel_path[1:]
                # Save to the actual directory being browsed (absolute path)
                dest_dir = os.path.join(ROOT_DIR, rel_path) if rel_path else ROOT_DIR
                if not os.path.exists(dest_dir):
                    os.makedirs(dest_dir)
                filepath = os.path.join(dest_dir, os.path.basename(filename))
                with open(filepath, 'wb') as fout:
                    fout.write(file_data)
                size_kb = os.path.getsize(filepath) // 1024
                print(f'Uploaded: {os.path.basename(filename)} ({size_kb}KB)', flush=True)
            
            self.send_response(303)
            self.send_header('Location', self.path)
            self.end_headers()
        except Exception as e:
            print(f'Error: {e}', flush=True)
            self.send_response(500)
            self.end_headers()

    def list_directory(self, path):
        try:
            entries = os.listdir(path)
        except OSError:
            self.send_error(404, 'No permission to list directory')
            return None
        
        # Separate dirs and files, sort each
        dirs = sorted([e for e in entries if os.path.isdir(os.path.join(path, e))], key=str.lower)
        files = sorted([e for e in entries if not os.path.isdir(os.path.join(path, e))], key=str.lower)
        entries = dirs + files
        
        currentpath = urllib.parse.unquote(self.path)
        
        # Build breadcrumb
        breadcrumb = '<a href="/" style="color:#00ffff">&#127968; /storage</a>'
        parts = [p for p in currentpath.split('/') if p]
        cumulative = ''
        for part in parts:
            cumulative += '/' + part
            breadcrumb += f' / <a href="{urllib.parse.quote(cumulative)}/" style="color:#00ffff">{html.escape(part)}</a>'
        
        content = '<!DOCTYPE html><html><head>'
        content += '<meta charset="utf-8">'
        content += '<title>ROM Transfer - ' + html.escape(currentpath) + '</title>'
        content += '<meta name="viewport" content="width=device-width, initial-scale=1">'
        content += '<style>'
        content += 'body{font-family:monospace;background:#0d0d1a;color:#e0e0ff;padding:10px;margin:0;}'
        content += 'h2{color:#00ffff;margin:0 0 5px 0;font-size:14px;}'
        content += '.breadcrumb{background:#1a1a2e;padding:8px 10px;margin-bottom:10px;font-size:13px;border-left:3px solid #00ffff;}'
        content += 'a{color:#00ffff;text-decoration:none;}a:hover{color:#ff00ff;}'
        content += '.upload-box{background:#1a1a2e;padding:10px;margin-bottom:10px;border:1px dashed #00ffff;font-size:13px;}'
        content += '.upload-box input[type=file]{color:white;font-size:12px;}'
        content += 'input[type=submit]{background:#00ffff;color:#000;border:none;padding:6px 12px;font-weight:bold;cursor:pointer;margin-top:5px;}'
        content += 'table{width:100%;border-collapse:collapse;font-size:13px;}'
        content += 'tr:hover{background:#1a1a2e;}'
        content += 'td{padding:6px 4px;border-bottom:1px solid #222;}'
        content += 'td:last-child{color:#888;text-align:right;white-space:nowrap;}'
        content += '.dir{color:#ffe066;}'
        content += '</style></head><body>'
        content += '<div class="breadcrumb">' + breadcrumb + '</div>'
        content += f'<div class="upload-box">'
        content += f'<form enctype="multipart/form-data" method="post" action="{html.escape(self.path)}">'
        content += '<input type="file" name="file" required> '
        content += '<input type="submit" value="&#11014; Upload Here">'
        content += '</form></div>'
        content += '<table>'
        if currentpath not in ('/', ''):
            content += '<tr><td><a href="../">&#11014; ..</a></td><td></td></tr>'
        for name in entries:
            fullname = os.path.join(path, name)
            displayname = html.escape(name)
            linkname = urllib.parse.quote(name)
            if os.path.isdir(fullname):
                size_str = ''
                content += f'<tr><td><span class="dir">&#128193;</span> <a href="{linkname}/" class="dir">{displayname}/</a></td><td>{size_str}</td></tr>'
            else:
                try:
                    size = os.path.getsize(fullname)
                    if size > 1048576:
                        size_str = f'{size//1048576} MB'
                    elif size > 1024:
                        size_str = f'{size//1024} KB'
                    else:
                        size_str = f'{size} B'
                except:
                    size_str = ''
                content += f'<tr><td>&#128196; <a href="{linkname}">{displayname}</a></td><td>{size_str}</td></tr>'
        content += '</table></body></html>'
        
        encoded = content.encode('utf-8')
        f = io.BytesIO(encoded)
        self.send_response(200)
        self.send_header('Content-type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(encoded)))
        self.end_headers()
        return f

with socketserver.TCPServer(("", PORT), ROMUploadHandler) as httpd:
    print(f"Serving at port {PORT}", flush=True)
    httpd.serve_forever()
)";

void writeServerScript() {
    FILE* f = fopen("/tmp/wt_server.py", "w");
    if (f) {
        fwrite(PY_SERVER, 1, strlen(PY_SERVER), f);
        fclose(f);
    }
}

std::string getIP() {
    std::string ip = "";
    // Try multiple methods for busybox/linux compatibility
    const char* cmds[] = {
        "ip route get 1.1.1.1 2>/dev/null | grep -oP 'src \\K\\S+'",
        "ip addr show wlan0 2>/dev/null | grep -w inet | awk '{print $2}' | cut -d/ -f1",
        "ifconfig wlan0 2>/dev/null | grep 'inet addr' | cut -d: -f2 | awk '{print $1}'",
        "hostname -I 2>/dev/null | awk '{print $1}'"
    };
    
    for (int i = 0; i < 4; ++i) {
        FILE* fp = popen(cmds[i], "r");
        if (fp) {
            char buf[64];
            if (fgets(buf, sizeof(buf), fp) != nullptr) {
                ip = buf;
                while (!ip.empty() && (ip.back() == '\n' || ip.back() == '\r' || ip.back() == ' '))
                    ip.pop_back();
            }
            pclose(fp);
            if (!ip.empty()) break;
        }
    }
    return ip;
}

void startServer() {
    app.ip = getIP();
    if (app.ip.empty()) {
        app.log_lines.push_back("ERROR: No Wi-Fi connection!");
        return; // Don't start
    }
    
    writeServerScript();
    
    // Use unbuffered output for python to get realtime logs
    app.server_out = popen("PYTHONUNBUFFERED=1 python3 /tmp/wt_server.py 2>&1", "r");
    if (app.server_out) {
        int fd = fileno(app.server_out);
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        app.state = STATE_SERVING;
        app.log_lines.clear();
        app.log_lines.push_back("Server started.");
    }
}

void stopServer() {
    if (app.server_out) {
        // popen returns a stream, we can't easily get pid. 
        // We just kill all python3 running our script
        system("pkill -f 'python3 /tmp/wt_server.py'");
        pclose(app.server_out);
        app.server_out = nullptr;
    }
    app.state = STATE_IDLE;
}

void pollServerLog() {
    if (app.state == STATE_SERVING && app.server_out) {
        char buf[256];
        while (fgets(buf, sizeof(buf), app.server_out) != nullptr) {
            std::string line = buf;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
            if (!line.empty()) {
                app.log_lines.push_back(line);
                if (app.log_lines.size() > 12) {
                    app.log_lines.erase(app.log_lines.begin());
                }
            }
        }
    }
}

void drawCyberpunkHUD() {
    SDL_SetRenderDrawColor(g_renderer, COLOR_BODY_BG, 255);
    SDL_RenderClear(g_renderer);
    
    // Header
    SDL_Rect header = {0, 0, SCREEN_WIDTH, 40};
    SDL_SetRenderDrawColor(g_renderer, COLOR_TITLE_BG, 255);
    SDL_RenderFillRect(g_renderer, &header);
    
    // Header Border (Cyan)
    SDL_SetRenderDrawColor(g_renderer, COLOR_TEXT_CYAN, 255);
    SDL_RenderDrawLine(g_renderer, 0, 40, SCREEN_WIDTH, 40);
    
    // Footer Border
    SDL_RenderDrawLine(g_renderer, 0, SCREEN_HEIGHT - 40, SCREEN_WIDTH, SCREEN_HEIGHT - 40);
    
    drawText(font_texture, cdata, ">>> WIFI ROM TRANSFER", 10, 28, {COLOR_TEXT_CYAN});
    
    std::string status = (app.state == STATE_SERVING) ? "[STATUS: SERVING]" : "[STATUS: IDLE]";
    SDL_Color statusColor = (app.state == STATE_SERVING) ? SDL_Color{COLOR_TEXT_YELLOW} : SDL_Color{COLOR_TEXT_NORMAL};
    drawText(font_texture, cdata, status, SCREEN_WIDTH - 200, 28, statusColor);
    
    // Footer text
    std::string footerText;
    if (app.state == STATE_IDLE) {
        footerText = "[A] START SERVER     [B] EXIT";
    } else if (app.state == STATE_SERVING) {
        footerText = "[X/Y] MENU     [B] STOP & EXIT";
    } else if (app.state == STATE_MENU) {
        footerText = "[A/X] SELECT     [B] CLOSE MENU";
    }
    drawText(font_texture, cdata, footerText, 10, SCREEN_HEIGHT - 12, {COLOR_TEXT_MAGENTA});
}

void drawIdle() {
    drawText(font_texture, cdata, "Please connect to Wi-Fi before starting.", 20, 80, {COLOR_TEXT_NORMAL});
    
    std::string ip = getIP();
    if (ip.empty()) {
        drawText(font_texture, cdata, "[NO IP FOUND - CHECK WI-FI CONNECTION]", 20, 120, {COLOR_TEXT_RED});
    } else {
        drawText(font_texture, cdata, "Current IP: " + ip, 20, 120, {COLOR_TEXT_CYAN});
        drawText(font_texture, cdata, "Press [A] to start server.", 20, 160, {COLOR_TEXT_YELLOW});
    }
}

void drawServing() {
    // Blinking URL
    static Uint32 last_blink = 0;
    static bool blink_state = true;
    if (SDL_GetTicks() - last_blink > 500) {
        blink_state = !blink_state;
        last_blink = SDL_GetTicks();
    }
    
    std::string url = "http://" + app.ip + ":" + std::to_string(SERVER_PORT);
    if (blink_state) {
        drawText(font_texture, cdata, "ACCESS URL: " + url, 20, 80, {COLOR_TEXT_YELLOW});
    }
    
    drawText(font_texture, cdata, "--- REQUEST LOG ---", 20, 120, {COLOR_TEXT_CYAN});
    
    // Draw logs
    int y = 150;
    for (const auto& line : app.log_lines) {
        drawText(font_texture_mono, cdata_mono, line, 20, y, {COLOR_TEXT_NORMAL});
        y += 20;
    }
}

void drawMenu() {
    // Dim background
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 150);
    SDL_Rect full = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
    SDL_RenderFillRect(g_renderer, &full);
    
    // Menu box
    SDL_Rect box = {SCREEN_WIDTH/2 - 150, SCREEN_HEIGHT/2 - 60, 300, 120};
    SDL_SetRenderDrawColor(g_renderer, COLOR_BODY_BG, 255);
    SDL_RenderFillRect(g_renderer, &box);
    SDL_SetRenderDrawColor(g_renderer, COLOR_TEXT_CYAN, 255);
    SDL_RenderDrawRect(g_renderer, &box);
    
    drawText(font_texture, cdata, "OPTIONS:", box.x + 10, box.y + 30, {COLOR_TEXT_CYAN});
    
    std::string opt0 = "[0] CANCEL";
    std::string opt1 = "[1] STOP SERVER";
    
    if (app.menu_sel == 0) {
        SDL_Rect sel = {box.x + 10, box.y + 40, 280, 30};
        SDL_SetRenderDrawColor(g_renderer, COLOR_SELECTION_BG, 255);
        SDL_RenderFillRect(g_renderer, &sel);
        drawText(font_texture, cdata, opt0, box.x + 20, box.y + 62, {COLOR_TEXT_YELLOW});
    } else {
        drawText(font_texture, cdata, opt0, box.x + 20, box.y + 62, {COLOR_TEXT_NORMAL});
    }
    
    if (app.menu_sel == 1) {
        SDL_Rect sel = {box.x + 10, box.y + 70, 280, 30};
        SDL_SetRenderDrawColor(g_renderer, COLOR_SELECTION_BG, 255);
        SDL_RenderFillRect(g_renderer, &sel);
        drawText(font_texture, cdata, opt1, box.x + 20, box.y + 92, {COLOR_TEXT_YELLOW});
    } else {
        drawText(font_texture, cdata, opt1, box.x + 20, box.y + 92, {COLOR_TEXT_NORMAL});
    }
}

void handleInput(SDL_Event& event) {
    if (event.type == SDL_QUIT) {
        app.quit = true;
        return;
    }
    
    if (app.state == STATE_IDLE) {
        if (BUTTON_PRESSED_B) {
            app.quit = true;
        } else if (BUTTON_PRESSED_A) {
            startServer();
        }
    } 
    else if (app.state == STATE_SERVING) {
        if (BUTTON_PRESSED_B) {
            stopServer();
            app.quit = true;
        } else if (BUTTON_PRESSED_X || BUTTON_PRESSED_Y) {
            app.state = STATE_MENU;
            app.menu_sel = 0;
        }
    }
    else if (app.state == STATE_MENU) {
        if (BUTTON_PRESSED_UP) {
            app.menu_sel = 0;
        } else if (BUTTON_PRESSED_DOWN) {
            app.menu_sel = 1;
        } else if (BUTTON_PRESSED_B) {
            app.state = STATE_SERVING;
        } else if (BUTTON_PRESSED_A || BUTTON_PRESSED_X) {
            if (app.menu_sel == 0) {
                app.state = STATE_SERVING;
            } else if (app.menu_sel == 1) {
                stopServer();
                app.quit = true; // Thoát hẳn khi chọn STOP
            }
        }
    }
}

int main(int argc, char* argv[]) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK);
    
    // Golden Fix cho Audio (đề phòng)
    system("echo 0xf4 > /sys/devices/platform/soc/1c22c00.codec/sunxi_pcm_codec_reg_update 2>/dev/null || true");
    
    if (SDL_NumJoysticks() > 0) {
        g_joystick = SDL_JoystickOpen(0);
    }
    
    Uint32 flags = 0;
    if (FULLSCREEN) flags |= SDL_WINDOW_FULLSCREEN;
    g_window = SDL_CreateWindow(APP_NAME, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH, SCREEN_HEIGHT, flags);
    
    Uint32 render_flags = SDL_RENDERER_SOFTWARE;
    if (HARDWARE_ACCELERATION) render_flags = SDL_RENDERER_ACCELERATED;
    g_renderer = SDL_CreateRenderer(g_window, -1, render_flags);
    
    std::string res_path = "./res/";
    #ifdef RES_PATH
        res_path = RES_PATH;
        if (res_path.back() != '/') res_path += "/";
    #endif
    
    initFont(res_path + FONT_NAME, &font_texture, cdata);
    initFont(res_path + FONT_NAME_MONO, &font_texture_mono, cdata_mono);
    
    SDL_Event e;
    while (!app.quit) {
        while (SDL_PollEvent(&e)) {
            handleInput(e);
        }
        
        pollServerLog();
        
        drawCyberpunkHUD();
        if (app.state == STATE_IDLE) {
            drawIdle();
        } else if (app.state == STATE_SERVING) {
            drawServing();
        } else if (app.state == STATE_MENU) {
            drawServing();
            drawMenu();
        }
        
        SDL_RenderPresent(g_renderer);
        SDL_Delay(MS_PER_FRAME);
    }
    
    stopServer(); // Cleanup
    
    if (font_texture) SDL_DestroyTexture(font_texture);
    if (font_texture_mono) SDL_DestroyTexture(font_texture_mono);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    if (g_joystick) SDL_JoystickClose(g_joystick);
    SDL_Quit();
    
    // Golden Fix trước khi exit
    system("echo 0xf4 > /sys/devices/platform/soc/1c22c00.codec/sunxi_pcm_codec_reg_update 2>/dev/null || true");
    return 0;
}
