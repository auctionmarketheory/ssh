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
import cgi
import urllib.parse

PORT = 8000
ROMS_DIR = '/storage/roms' if os.path.exists('/storage/roms') else ('/roms' if os.path.exists('/roms') else '/tmp/roms')
if not os.path.exists(ROMS_DIR):
    os.makedirs(ROMS_DIR)
os.chdir(ROMS_DIR)

class ROMUploadHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        print(f"[{self.command}] {self.path} {args[1]}", flush=True)

    def do_GET(self):
        if self.path.startswith('/?'):
            # Clear query string from URL for clean navigation
            self.send_response(301)
            self.send_header('Location', self.path.split('?')[0])
            self.end_headers()
            return
            
        f = self.send_head()
        if f:
            try:
                self.copyfile(f, self.wfile)
            finally:
                f.close()

    def do_POST(self):
        try:
            form = cgi.FieldStorage(
                fp=self.rfile,
                headers=self.headers,
                environ={'REQUEST_METHOD': 'POST',
                         'CONTENT_TYPE': self.headers['Content-Type'],
                         }
            )
            
            if 'file' in form:
                file_item = form['file']
                if file_item.filename:
                    # Save to current requested directory
                    rel_path = urllib.parse.unquote(self.path)
                    if rel_path.startswith('/'):
                        rel_path = rel_path[1:]
                    
                    dest_dir = os.path.join(ROMS_DIR, rel_path)
                    if not os.path.exists(dest_dir):
                        os.makedirs(dest_dir)
                        
                    filename = os.path.basename(file_item.filename)
                    filepath = os.path.join(dest_dir, filename)
                    
                    with open(filepath, 'wb') as fout:
                        while True:
                            chunk = file_item.file.read(8192)
                            if not chunk:
                                break
                            fout.write(chunk)
                    
                    size_kb = os.path.getsize(filepath) // 1024
                    print(f"Uploaded: {filename} ({size_kb}KB) to {rel_path}", flush=True)
            
            self.send_response(303)
            self.send_header('Location', self.path)
            self.end_headers()
        except Exception as e:
            print(f"Error: {e}", flush=True)
            self.send_response(500)
            self.end_headers()

    def list_directory(self, path):
        try:
            list = os.listdir(path)
        except OSError:
            self.send_error(404, "No permission to list directory")
            return None
        list.sort(key=lambda a: a.lower())
        
        displaypath = cgi.escape(urllib.parse.unquote(self.path))
        
        f = open('/tmp/html_resp.html', 'w', encoding='utf-8')
        f.write('<!DOCTYPE html><html><head><title>ROM Transfer</title>')
        f.write('<meta name="viewport" content="width=device-width, initial-scale=1">')
        f.write('<style>body{font-family:sans-serif; background:#2D2D2D; color:white; padding:20px;} ')
        f.write('a{color:#00ffff; text-decoration:none;} a:hover{color:#ff00ff;} ')
        f.write('.upload-box{background:#444; padding:15px; margin-bottom:20px; border:2px dashed #00ffff;} ')
        f.write('li{padding:8px 0; border-bottom:1px solid #555; list-style:none;} ')
        f.write('ul{padding-left:0;}</style></head><body>')
        f.write(f'<h2>📁 {displaypath}</h2>')
        
        f.write('<div class="upload-box">')
        f.write(f'<form enctype="multipart/form-data" method="post" action="{self.path}">')
        f.write('<input type="file" name="file" required><br><br>')
        f.write('<input type="submit" value="UPLOAD TAI DAY" style="background:#00ffff; border:none; padding:10px; font-weight:bold; cursor:pointer;">')
        f.write('</form></div>')
        
        f.write('<ul>')
        if self.path != '/':
            f.write('<li><a href="..">⬆️ [Thu muc cha]</a></li>')
        for name in list:
            fullname = os.path.join(path, name)
            displayname = linkname = name
            if os.path.isdir(fullname):
                displayname = "📁 " + name + "/"
                linkname = name + "/"
            else:
                displayname = "📄 " + name
            f.write(f'<li><a href="{urllib.parse.quote(linkname)}">{cgi.escape(displayname)}</a></li>')
        f.write('</ul></body></html>')
        f.close()
        
        f = open('/tmp/html_resp.html', 'rb')
        self.send_response(200)
        self.send_header("Content-type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(os.path.getsize('/tmp/html_resp.html')))
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
    FILE* fp = popen("hostname -I | awk '{print $1}'", "r");
    if (fp) {
        char buf[64];
        if (fgets(buf, sizeof(buf), fp) != nullptr) {
            ip = buf;
            // Trim newline
            while (!ip.empty() && (ip.back() == '\n' || ip.back() == '\r' || ip.back() == ' '))
                ip.pop_back();
        }
        pclose(fp);
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
        footerText = "[A] BAT SERVER     [B] THOAT";
    } else if (app.state == STATE_SERVING) {
        footerText = "[X/Y] MENU     [B] STOP & THOAT";
    } else if (app.state == STATE_MENU) {
        footerText = "[A/X] CHON     [B] DONG MENU";
    }
    drawText(font_texture, cdata, footerText, 10, SCREEN_HEIGHT - 12, {COLOR_TEXT_MAGENTA});
}

void drawIdle() {
    drawText(font_texture, cdata, "Ket noi Wi-Fi truoc khi bat server.", 20, 80, {COLOR_TEXT_NORMAL});
    
    std::string ip = getIP();
    if (ip.empty()) {
        drawText(font_texture, cdata, "[KHONG TIM THAY IP - VUI LONG KET NOI WIFI]", 20, 120, {COLOR_TEXT_RED});
    } else {
        drawText(font_texture, cdata, "IP Hien Tai: " + ip, 20, 120, {COLOR_TEXT_CYAN});
        drawText(font_texture, cdata, "Bam [A] de bat server.", 20, 160, {COLOR_TEXT_YELLOW});
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
        drawText(font_texture, cdata, "TRUY CAP: " + url, 20, 80, {COLOR_TEXT_YELLOW});
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
