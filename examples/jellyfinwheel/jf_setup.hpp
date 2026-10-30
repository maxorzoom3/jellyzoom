// The sign-in pages: find or type a server, then sign in as a profile, with a password, or with Quick Connect.
// Drawn over the loading screen while the gate waits for a session (jf_gate_auth_state() is 1 or 2).
// Needs kit.hpp (hui::) and jf_gate.h before it.
#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include "jf_qr.hpp"

namespace jfsetup {

struct Cell {
	std::string label;
	char ch{ 0 };    // the character it types, or 0 for a control
	int control{ 0 }; // 1 shift, 2 symbols, 3 space, 4 delete, 5 done
	int col{ 0 }, span{ 1 };
};

class Screen
{
  public:
	enum class Page { servers, signin, quick };
	enum class Purpose { address, user, password };

	// One frame: reads the pad, talks to the gate through jf_gate_auth_*
	template <class Feedback>
	void update(const hui::InputFrame &in, float dt, Feedback &fb)
	{
		clock_ += dt;
		refresh(dt);
		const int state = jf_gate_auth_state();
		busy_ = jf_gate_auth_busy() != 0;
		if (!started_ && state != 0) {
			started_ = true;
			page_ = state == 2 && !chooseOnStart_ ? Page::signin : Page::servers;
			choosing_ = chooseOnStart_ && state == 2;
			chooseOnStart_ = false;
			if (page_ == Page::servers) {
				jf_gate_auth_discover();
			}
			rebuildMenu();
		}
		if (!started_) {
			return;
		}
		// The server was accepted while the person was on the first page
		if (state == 2 && page_ == Page::servers && !keyboardOpen_ && !choosing_ && !waiting_) {
			page_ = Page::signin;
			rebuildMenu();
		}
		if (state == 1 && page_ != Page::servers && !keyboardOpen_) {
			page_ = Page::servers;
			rebuildMenu();
		}
		if (waiting_ && !busy_) {
			waiting_ = false;
			char now[256];
			jf_gate_auth_text(0, now, sizeof(now));
			message_ = now;
			if (!message_.empty()) {
				fb.play(hui::audio::Cue::error);
			} else if (choosing_ && jf_gate_auth_state() == 2) {
				choosing_ = false;
				page_ = Page::signin;
				sel_ = 0;
				rebuildMenu();
			}
		}
		if (busy_ || waiting_) {
			return;
		}
		if (keyboardOpen_) {
			keyboard(in, fb);
			return;
		}
		if (page_ == Page::quick) {
			quick(in, fb);
			return;
		}
		menu(in, fb);
	}

	// Switch server: the pages open on the server chooser, with a way back to the server that is already set
	void chooseServer() { chooseOnStart_ = true; }

	void draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts) const
	{
		using hui::gfx::Color;
		using hui::gfx::Rect;
		using hui::gfx::Align;
		const Color white{ 1, 1, 1, 1 };
		list.rounded_rect(Rect{ 0, 0, 1920, 1080 }, 0.0f, Color{ 0.02f, 0.025f, 0.05f, 0.88f });
		hui::ui::text(list, fonts.display, "Jellyzoom", 960.0f, 150.0f, 64, white, Align::center);
		if (!started_) {
			hui::ui::text(list, fonts.regular, "Connecting ...", 960.0f, 540.0f, 40, white.with_alpha(0.7f), Align::center);
			return;
		}
		const char *heading = page_ == Page::servers ? "Choose your Jellyfin server"
			: page_ == Page::quick ? "Quick Connect" : "Who is watching?";
		if (keyboardOpen_) {
			heading = purpose_ == Purpose::address ? "Server address" : purpose_ == Purpose::user ? "User name" : "Password";
		}
		hui::ui::text(list, fonts.semibold, heading, 960.0f, 230.0f, 44, white.with_alpha(0.92f), Align::center);
		if (page_ == Page::signin && !keyboardOpen_ && !serverName_.empty()) {
			hui::ui::text(list, fonts.regular, serverName_.c_str(), 960.0f, 275.0f, 28, white.with_alpha(0.55f), Align::center);
		}
		if (!message_.empty()) {
			hui::ui::text(list, fonts.regular, message_.c_str(), 960.0f, 320.0f, 30, Color{ 1.0f, 0.55f, 0.5f, 1.0f }, Align::center);
		}
		if (!keyboardOpen_ && (certOff_ || (plain_ && page_ == Page::signin))) {
			const Color amber{ 1.0f, 0.78f, 0.35f, 0.9f };
			if (certOff_) {
				hui::ui::text(list, fonts.regular, "Certificate check is OFF (/data/jellyzoom-tls-insecure): https servers are not verified.", 960.0f, 1010.0f, 24, amber, Align::center);
			}
			if (plain_ && page_ == Page::signin) {
				hui::ui::text(list, fonts.regular, "This server is not encrypted (http): a password is sent as plain text. Quick Connect or https is safer.", 960.0f, certOff_ ? 1044.0f : 1020.0f, 24, amber, Align::center);
			}
		}
		if (busy_ || waiting_) {
			const float a = 0.55f + 0.35f * std::sin(clock_ * 3.0f);
			hui::ui::text(list, fonts.display, "One moment ...", 960.0f, 600.0f, 48, white.with_alpha(a), Align::center);
			return;
		}
		if (keyboardOpen_) {
			drawKeyboard(list, fonts);
			return;
		}
		if (page_ == Page::quick) {
			drawQuick(list, fonts);
			return;
		}
		drawMenu(list, fonts);
	}

  private:
	struct Item {
		std::string label, detail;
		int kind{ 0 }; // servers: 1 found server, 2 type an address; sign-in: 1 profile, 2 other user, 3 quick connect, 4 change server
		std::string host;
		int port{ 8096 };
	};

	Page page_{ Page::servers };
	Purpose purpose_{ Purpose::address };
	bool started_{ false }, busy_{ false }, waiting_{ false }, keyboardOpen_{ false };
	bool choosing_{ false }, chooseOnStart_{ false }; // choosing another server while one is already set
	float clock_{ 0.0f }, refreshIn_{ 0.0f };
	std::vector<Item> items_;
	int sel_{ 0 };
	std::string message_, serverName_, quickCode_, quickUrl_;
	mutable std::string qrText_;
	mutable jfqr::Code qrCode_;
	int quickState_{ 0 };
	bool plain_{ false }, certOff_{ false }; // the server is reached without encryption / the certificate check is off
	int searching_{ 0 };
	size_t foundCount_{ 0 };

	// keyboard
	std::string text_, user_, address_{ "192.168.1." };
	bool showText_{ false };
	int kbPage_{ 0 }; // 0 lower, 1 upper, 2 symbols
	int kbRow_{ 1 }, kbCol_{ 0 };
	std::vector<std::vector<Cell>> rows_;

	void refresh(float dt)
	{
		refreshIn_ -= dt;
		if (refreshIn_ > 0.0f) {
			return;
		}
		refreshIn_ = 0.2f;
		char buf[256];
		jf_gate_auth_text(0, buf, sizeof(buf));
		message_ = buf;
		jf_gate_auth_text(1, buf, sizeof(buf));
		serverName_ = buf;
		jf_gate_auth_text(3, buf, sizeof(buf));
		quickCode_ = buf;
		jf_gate_auth_text(4, buf, sizeof(buf));
		quickUrl_ = buf;
		plain_ = quickUrl_.rfind("http://", 0) == 0;
		certOff_ = jf_gate_tls_insecure() != 0;
		quickState_ = jf_gate_auth_quick_state();
		searching_ = jf_gate_auth_discover_state();
		if (started_ && page_ == Page::servers && !keyboardOpen_) {
			char n[8], h[8];
			int p = 0;
			const size_t count = static_cast<size_t>(jf_gate_auth_found(0, n, 1, h, 1, &p));
			if (count != foundCount_) {
				foundCount_ = count;
				rebuildMenu();
			}
		}
	}

	void rebuildMenu()
	{
		items_.clear();
		if (page_ == Page::servers) {
			char name[128], host[64];
			int port = 8096;
			const int count = jf_gate_auth_found(0, name, sizeof(name), host, sizeof(host), &port);
			for (int i = 0; i < count && i < 6; i++) {
				jf_gate_auth_found(i, name, sizeof(name), host, sizeof(host), &port);
				Item it;
				it.label = name[0] ? name : host;
				it.detail = std::string(host) + ":" + std::to_string(port);
				it.kind = 1;
				it.host = host;
				it.port = port;
				items_.push_back(it);
			}
			Item typed;
			typed.label = "Type the address";
			typed.detail = "for example 192.168.1.10:8096";
			typed.kind = 2;
			items_.push_back(typed);
			if (jf_gate_auth_state() == 2) {
				Item stay;
				stay.label = "Back";
				stay.detail = serverName_.empty() ? "keep the server you had" : "keep " + serverName_;
				stay.kind = 3;
				items_.push_back(stay);
			}
		} else if (page_ == Page::signin) {
			char name[128];
			const int count = jf_gate_auth_users(0, name, sizeof(name));
			for (int i = 0; i < count; i++) {
				jf_gate_auth_users(i, name, sizeof(name));
				Item it;
				it.label = name;
				it.kind = 1;
				items_.push_back(it);
			}
			Item other;
			other.label = count > 0 ? "Another user" : "User name and password";
			other.kind = 2;
			items_.push_back(other);
			if (jf_gate_auth_quick_available() == 1) {
				Item quick;
				quick.label = "Quick Connect";
				quick.detail = "approve from another device";
				quick.kind = 3;
				items_.push_back(quick);
			}
			Item back;
			back.label = "Change server";
			back.kind = 4;
			items_.push_back(back);
		}
		sel_ = std::min(sel_, std::max(0, static_cast<int>(items_.size()) - 1));
	}

	template <class Feedback>
	void menu(const hui::InputFrame &in, Feedback &fb)
	{
		if (in.nav == hui::Direction::up && sel_ > 0) {
			sel_--;
			fb.play(hui::audio::Cue::focus);
		} else if (in.nav == hui::Direction::down && sel_ + 1 < static_cast<int>(items_.size())) {
			sel_++;
			fb.play(hui::audio::Cue::focus);
		}
		if (page_ == Page::servers && in.is_pressed(hui::Action::west)) {
			jf_gate_auth_discover(); // Square: look again
			fb.play(hui::audio::Cue::tab);
		}
		if (page_ == Page::servers && choosing_ && in.is_pressed(hui::Action::back)) {
			leaveChooser(fb);
			return;
		}
		if (!in.is_pressed(hui::Action::confirm) || items_.empty()) {
			return;
		}
		const Item &it = items_[static_cast<size_t>(sel_)];
		fb.play(hui::audio::Cue::open);
		message_.clear();
		if (page_ == Page::servers) {
			if (it.kind == 3) {
				leaveChooser(fb);
				return;
			}
			if (it.kind == 1) {
				address_ = it.host + ":" + std::to_string(it.port);
				waiting_ = true;
				jf_gate_auth_server(it.host.c_str(), it.port);
			} else {
				openKeyboard(Purpose::address, address_);
			}
			return;
		}
		switch (it.kind) {
		case 1:
			user_ = it.label;
			openKeyboard(Purpose::password, "");
			break;
		case 2:
			openKeyboard(Purpose::user, user_);
			break;
		case 3:
			page_ = Page::quick;
			jf_gate_auth_quick_start();
			break;
		case 4:
			page_ = Page::servers;
			choosing_ = true;
			message_.clear();
			sel_ = 0;
			jf_gate_auth_discover();
			foundCount_ = 0;
			rebuildMenu();
			break;
		}
	}

	// Back to the sign-in page of the server that is already set
	template <class Feedback>
	void leaveChooser(Feedback &fb)
	{
		fb.play(hui::audio::Cue::back);
		choosing_ = false;
		message_.clear();
		page_ = Page::signin;
		sel_ = 0;
		rebuildMenu();
	}

	template <class Feedback>
	void quick(const hui::InputFrame &in, Feedback &fb)
	{
		if (in.is_pressed(hui::Action::back)) {
			fb.play(hui::audio::Cue::back);
			jf_gate_auth_quick_cancel();
			page_ = Page::signin;
			rebuildMenu();
		}
	}

	void openKeyboard(Purpose p, const std::string &initial)
	{
		purpose_ = p;
		text_ = initial;
		keyboardOpen_ = true;
		showText_ = p != Purpose::password;
		kbPage_ = 0;
		kbRow_ = 1;
		kbCol_ = 0;
		buildKeys();
	}

	void buildKeys()
	{
		static const char *const lower[4] = { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._:" };
		static const char *const upper[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._:" };
		static const char *const sym[4] = { "!@#$%^&*()", "-_=+[]{};:", "'\"<>,.?/\\|", "~`@#.:/_-+" };
		const char *const *set = kbPage_ == 0 ? lower : kbPage_ == 1 ? upper : sym;
		rows_.assign(5, {});
		for (int r = 0; r < 4; r++) {
			for (int c = 0; c < 10; c++) {
				Cell cell;
				cell.ch = set[r][c];
				cell.label = std::string(1, cell.ch);
				cell.col = c;
				rows_[static_cast<size_t>(r)].push_back(cell);
			}
		}
		auto control = [&](const char *label, int kind, int col, int span) {
			Cell cell;
			cell.label = label;
			cell.control = kind;
			cell.col = col;
			cell.span = span;
			rows_[4].push_back(cell);
		};
		control(kbPage_ == 1 ? "abc" : "ABC", 1, 0, 2);
		control(kbPage_ == 2 ? "abc" : "#+=", 2, 2, 2);
		control("space", 3, 4, 3);
		control("delete", 4, 7, 2);
		control("done", 5, 9, 1);
		kbRow_ = std::min(kbRow_, 4);
		kbCol_ = std::min(kbCol_, 9);
	}

	int keyAt(int row, int col) const
	{
		const auto &r = rows_[static_cast<size_t>(row)];
		int best = 0, bestDistance = 99;
		for (int i = 0; i < static_cast<int>(r.size()); i++) {
			if (col >= r[static_cast<size_t>(i)].col && col < r[static_cast<size_t>(i)].col + r[static_cast<size_t>(i)].span) {
				return i;
			}
			const int d = std::abs(r[static_cast<size_t>(i)].col - col);
			if (d < bestDistance) {
				bestDistance = d;
				best = i;
			}
		}
		return best;
	}

	template <class Feedback>
	void finishKeyboard(Feedback &fb)
	{
		fb.play(hui::audio::Cue::open);
		keyboardOpen_ = false;
		if (purpose_ == Purpose::address) {
			address_ = text_;
			// an address, a name, or https://name (the program looks names up and does the TLS itself)
			std::string host = text_;
			std::string scheme;
			if (host.rfind("https://", 0) == 0) {
				scheme = "https://";
				host = host.substr(8);
			} else if (host.rfind("http://", 0) == 0) {
				scheme = "http://";
				host = host.substr(7);
			}
			while (!host.empty() && host.back() == '/') {
				host.pop_back();
			}
			int port = scheme == "https://" ? 443 : scheme == "http://" ? 80 : 8096;
			const size_t colon = host.rfind(':');
			if (colon != std::string::npos) {
				const int typed = std::atoi(host.c_str() + colon + 1);
				host = host.substr(0, colon);
				if (typed > 0 && typed <= 65535) {
					port = typed;
				}
			}
			host = scheme + host;
			waiting_ = true;
			jf_gate_auth_server(host.c_str(), port);
		} else if (purpose_ == Purpose::user) {
			user_ = text_;
			if (user_.empty()) {
				return;
			}
			openKeyboard(Purpose::password, "");
		} else {
			waiting_ = true;
			jf_gate_auth_password(user_.c_str(), text_.c_str());
			text_.clear();
		}
	}

	template <class Feedback>
	void keyboard(const hui::InputFrame &in, Feedback &fb)
	{
		if (in.is_pressed(hui::Action::back)) {
			fb.play(hui::audio::Cue::back);
			keyboardOpen_ = false;
			text_.clear();
			return;
		}
		if (in.is_pressed(hui::Action::west) && !text_.empty()) {
			text_.pop_back();
			fb.play(hui::audio::Cue::tick);
		}
		if (in.is_pressed(hui::Action::north) && text_.size() < 120) {
			text_ += ' ';
			fb.play(hui::audio::Cue::tick);
		}
		if (in.is_pressed(hui::Action::page_next)) {
			kbPage_ = kbPage_ == 1 ? 0 : 1;
			buildKeys();
			fb.play(hui::audio::Cue::tab);
		}
		if (in.is_pressed(hui::Action::page_prev)) {
			kbPage_ = kbPage_ == 2 ? 0 : 2;
			buildKeys();
			fb.play(hui::audio::Cue::tab);
		}
		if (in.is_pressed(hui::Action::l3) && purpose_ == Purpose::password) {
			showText_ = !showText_;
		}
		if (in.nav != hui::Direction::none) {
			const auto &row = rows_[static_cast<size_t>(kbRow_)];
			const int idx = keyAt(kbRow_, kbCol_);
			const Cell &now = row[static_cast<size_t>(idx)];
			if (in.nav == hui::Direction::left) {
				kbCol_ = row[static_cast<size_t>(idx > 0 ? idx - 1 : static_cast<int>(row.size()) - 1)].col;
			} else if (in.nav == hui::Direction::right) {
				kbCol_ = row[static_cast<size_t>(idx + 1 < static_cast<int>(row.size()) ? idx + 1 : 0)].col;
			} else if (in.nav == hui::Direction::up) {
				kbRow_ = kbRow_ > 0 ? kbRow_ - 1 : 4;
			} else if (in.nav == hui::Direction::down) {
				kbRow_ = kbRow_ < 4 ? kbRow_ + 1 : 0;
			}
			(void)now;
			fb.play(hui::audio::Cue::focus);
		}
		if (!in.is_pressed(hui::Action::confirm)) {
			return;
		}
		const Cell &cell = rows_[static_cast<size_t>(kbRow_)][static_cast<size_t>(keyAt(kbRow_, kbCol_))];
		switch (cell.control) {
		case 0:
			if (text_.size() < 120) {
				text_ += cell.ch;
			}
			fb.play(hui::audio::Cue::tick);
			break;
		case 1:
			kbPage_ = kbPage_ == 1 ? 0 : 1;
			buildKeys();
			break;
		case 2:
			kbPage_ = kbPage_ == 2 ? 0 : 2;
			buildKeys();
			break;
		case 3:
			text_ += ' ';
			fb.play(hui::audio::Cue::tick);
			break;
		case 4:
			if (!text_.empty()) {
				text_.pop_back();
			}
			fb.play(hui::audio::Cue::tick);
			break;
		case 5:
			finishKeyboard(fb);
			break;
		}
	}

	void drawMenu(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts) const
	{
		using hui::gfx::Color;
		using hui::gfx::Rect;
		using hui::gfx::Align;
		float y = 380.0f;
		for (int i = 0; i < static_cast<int>(items_.size()); i++) {
			const Item &it = items_[static_cast<size_t>(i)];
			const Rect box{ 560.0f, y, 800.0f, 84.0f };
			const bool on = i == sel_;
			list.bordered_rect(box, 20.0f, on ? Color{ 0.30f, 0.34f, 0.62f, 0.95f } : Color{ 1, 1, 1, 0.06f }, on ? 2.5f : 1.0f,
				Color{ 1, 1, 1, on ? 0.85f : 0.14f });
			hui::ui::text(list, fonts.semibold, it.label.c_str(), box.x + 36.0f, box.y + (it.detail.empty() ? 55.0f : 40.0f), 36,
				Color{ 1, 1, 1, on ? 1.0f : 0.8f });
			if (!it.detail.empty()) {
				hui::ui::text(list, fonts.regular, it.detail.c_str(), box.x + 36.0f, box.y + 70.0f, 24, Color{ 1, 1, 1, 0.55f });
			}
			y += 98.0f;
		}
		if (page_ == Page::servers) {
			const char *line = searching_ == 1 ? "Looking for servers on your network ..."
				: choosing_ ? "Square: search again    Circle: back" : "Square: search again";
			hui::ui::text(list, fonts.regular, line, 960.0f, y + 40.0f, 28, Color{ 1, 1, 1, 0.55f }, Align::center);
		}
	}

	void drawQr(hui::gfx::DrawList &list, const hui::gfx::Rect &box, const std::string &url) const
	{
		using hui::gfx::Color;
		using hui::gfx::Rect;
		if (qrText_ != url) {
			qrCode_ = jfqr::encode(url);
			qrText_ = url;
		}
		if (qrCode_.size <= 0) {
			return;
		}
		list.rounded_rect(box, 22.0f, Color{ 1, 1, 1, 1 });
		const int n = qrCode_.size;
		const int px = std::max(1, static_cast<int>(std::floor(box.w / static_cast<float>(n + 6))));
		const float side = static_cast<float>(px * n);
		const float ox = box.x + (box.w - side) * 0.5f;
		const float oy = box.y + (box.h - side) * 0.5f;
		for (int row = 0; row < n; ++row) {
			int col = 0;
			while (col < n) {
				if (!qrCode_.dark(col, row)) {
					++col;
					continue;
				}
				int end = col;
				while (end < n && qrCode_.dark(end, row)) {
					++end;
				}
				list.rounded_rect(Rect{ ox + static_cast<float>(col * px), oy + static_cast<float>(row * px),
										static_cast<float>((end - col) * px), static_cast<float>(px) },
					0.0f, Color{ 0, 0, 0, 1 });
				col = end;
			}
		}
	}

	void drawQuick(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts) const
	{
		using hui::gfx::Color;
		using hui::gfx::Align;
		if (quickState_ == 1 && !quickCode_.empty()) {
			std::string spaced;
			for (size_t i = 0; i < quickCode_.size(); i++) {
				spaced += quickCode_[i];
				if (i + 1 < quickCode_.size()) {
					spaced += ' ';
				}
			}
			const float cx = quickUrl_.empty() ? 960.0f : 700.0f; // the code, and beside it the QR for the phone
			hui::ui::text(list, fonts.display, spaced.c_str(), cx, 560.0f, quickUrl_.empty() ? 150 : 120, Color{ 1, 1, 1, 1 }, Align::center);
			hui::ui::text(list, fonts.regular, "On a phone or computer signed in to Jellyfin:", cx, 680.0f, 30,
				Color{ 1, 1, 1, 0.75f }, Align::center);
			hui::ui::text(list, fonts.regular, "open your profile > Quick Connect and type this code.", cx, 724.0f, 30,
				Color{ 1, 1, 1, 0.75f }, Align::center);
			if (!quickUrl_.empty()) {
				drawQr(list, hui::gfx::Rect{ 1170.0f, 400.0f, 320.0f, 320.0f }, quickUrl_);
				hui::ui::text(list, fonts.regular, "Or scan this to open Quick Connect", 1330.0f, 770.0f, 26, Color{ 1, 1, 1, 0.75f },
					Align::center);
				hui::ui::text(list, fonts.regular, "on your phone, then type the code.", 1330.0f, 804.0f, 26, Color{ 1, 1, 1, 0.75f },
					Align::center);
			}
		} else if (quickState_ == -1) {
			hui::ui::text(list, fonts.regular, "That did not work. Press Circle to go back.", 960.0f, 560.0f, 36,
				Color{ 1, 1, 1, 0.75f }, Align::center);
		} else {
			hui::ui::text(list, fonts.regular, "Asking the server for a code ...", 960.0f, 560.0f, 36, Color{ 1, 1, 1, 0.75f },
				Align::center);
		}
		hui::ui::text(list, fonts.regular, "Circle: back", 960.0f, 960.0f, 26, Color{ 1, 1, 1, 0.5f }, Align::center);
	}

	void drawKeyboard(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts) const
	{
		using hui::gfx::Color;
		using hui::gfx::Rect;
		using hui::gfx::Align;
		// the text so far
		const Rect field{ 440.0f, 350.0f, 1040.0f, 84.0f };
		list.bordered_rect(field, 18.0f, Color{ 1, 1, 1, 0.07f }, 1.5f, Color{ 1, 1, 1, 0.3f });
		std::string shown = text_;
		if (!showText_) {
			shown.assign(text_.size(), '*');
		}
		if (shown.size() > 34) {
			shown = shown.substr(shown.size() - 34);
		}
		hui::ui::text(list, fonts.semibold, shown.c_str(), field.x + 32.0f, field.y + 58.0f, 42, Color{ 1, 1, 1, 1 });
		const float width = shown.empty() ? 0.0f : fonts.semibold.measure(shown.c_str(), 42);
		const float blink = 0.5f + 0.5f * std::sin(clock_ * 7.0f);
		list.rounded_rect(Rect{ field.x + 36.0f + width, field.y + 20.0f, 3.0f, 44.0f }, 1.5f, Color{ 1, 1, 1, 0.35f + 0.55f * blink });
		// the keys
		const float kw = 96.0f, kh = 84.0f, gap = 8.0f;
		const float x0 = 960.0f - (10.0f * (kw + gap) - gap) * 0.5f;
		const float y0 = 470.0f;
		for (int r = 0; r < 5; r++) {
			for (const Cell &c : rows_[static_cast<size_t>(r)]) {
				const Rect box{ x0 + c.col * (kw + gap), y0 + r * (kh + gap), c.span * kw + (c.span - 1) * gap, kh };
				const bool on = r == kbRow_ && &c == &rows_[static_cast<size_t>(r)][static_cast<size_t>(keyAt(kbRow_, kbCol_))];
				const bool lit = c.control == 5;
				list.bordered_rect(box, 16.0f,
					on ? Color{ 0.30f, 0.34f, 0.62f, 0.98f } : lit ? Color{ 0.67f, 0.36f, 0.76f, 0.55f } : Color{ 1, 1, 1, 0.09f },
					on ? 2.5f : 1.0f, Color{ 1, 1, 1, on ? 0.9f : 0.14f });
				hui::ui::text(list, fonts.semibold, c.label.c_str(), box.cx(), box.y + 56.0f, c.control ? 28 : 36,
					Color{ 1, 1, 1, on ? 1.0f : 0.85f }, Align::center);
			}
		}
		const char *hint = purpose_ == Purpose::password
			? "Cross type   Square delete   Triangle space   L1 symbols   R1 capitals   L3 show   Circle back"
			: "Cross type   Square delete   Triangle space   L1 symbols   R1 capitals   Circle back";
		hui::ui::text(list, fonts.regular, hint, 960.0f, 1000.0f, 24, Color{ 1, 1, 1, 0.5f }, Align::center);
	}
};

} // namespace jfsetup
