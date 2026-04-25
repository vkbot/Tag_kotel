#pragma once
#include <Arduino.h>
#include <GSON.h>

#include "../api.h"
#include "../packet.h"

namespace fb {

enum class KeyStyle {
    Default,
    Danger,
    Success,
    Primary,
};

// https://core.telegram.org/bots/api#replykeyboardmarkup
class KeyboardBase {
    friend class Message;

   public:
    KeyboardBase() {
        _kb('[');
    }

   protected:
    virtual void _toJson(Packet& p) = 0;

    void _addStyle(KeyStyle style) {
        switch (style) {
            case KeyStyle::Default: break;
            case KeyStyle::Danger: _kb[tg_api::style] = F("danger"); break;
            case KeyStyle::Success: _kb[tg_api::style] = F("success"); break;
            case KeyStyle::Primary: _kb[tg_api::style] = F("primary"); break;
        }
    }

    gson::Str _kb;

   private:
};

class InlineKeyboard : public KeyboardBase {
   public:
    // добавить кнопку {}
    InlineKeyboard& addButton(gson::Str& obj) {
        _kb += obj;
        return *this;
    }

    // добавить кнопку
    InlineKeyboard& addButton(Text text, Text data = Text(), KeyStyle style = KeyStyle::Default) {
        _kb('{');
        _kb[tg_api::text].escape(text);

        if (data.startsWith(F("http://")) ||
            data.startsWith(F("https://")) ||
            data.startsWith(F("tg://"))) {
            _kb[tg_api::url] = data;
        } else {
            _kb[tg_api::callback_data].escape(data);
        }

        _addStyle(style);

        _kb('}');
        return *this;
    }

    // перенести строку
    InlineKeyboard& newRow() {
        _kb(']');
        _kb('[');
        return *this;
    }

   private:
    void _toJson(Packet& p) override {
        p[tg_api::inline_keyboard]('[');
        p += _kb;
        p(']');
        p(']');
    }
};

class Keyboard : public KeyboardBase {
   public:
    // принудительно показывать клавиатуру
    bool persistent = persistentDefault;

    // уменьшить клавиатуру под количество кнопок
    bool resize = resizeDefault;

    // автоматически скрывать после нажатия
    bool oneTime = oneTimeDefault;

    // показывать только упомянутым в сообщении юзерам
    bool selective = selectiveDefault;

    // подсказка, показывается в поле ввода при открытой клавиатуре (до 64 символов)
    String placeholder = "";

    // добавить кнопку {}
    Keyboard& addButton(gson::Str& obj) {
        _kb += obj;
        return *this;
    }

    // добавить кнопку
    Keyboard& addButton(Text text, KeyStyle style = KeyStyle::Default) {
        _kb('{');
        _kb[tg_api::text].escape(text);
        _addStyle(style);
        _kb('}');
        return *this;
    }

    // перенести строку
    Keyboard& newRow() {
        _kb(']');
        _kb('[');
        return *this;
    }

    // ===================================

    // принудительно показывать клавиатуру (умолч. 0)
    static bool persistentDefault;

    // уменьшить клавиатуру под количество кнопок (умолч. 0)
    static bool resizeDefault;

    // автоматически скрывать после нажатия (умолч. 0)
    static bool oneTimeDefault;

    // показывать только упомянутым в сообщении юзерам (умолч. 0)
    static bool selectiveDefault;

   private:
    void _toJson(Packet& p) override {
        p[tg_api::keyboard]('[');
        p += _kb;
        p(']');
        p(']');

        if (persistent) p[tg_api::is_persistent] = true;
        if (resize) p[tg_api::resize_keyboard] = true;
        if (oneTime) p[tg_api::one_time_keyboard] = true;
        if (selective) p[tg_api::selective] = true;
        if (placeholder.length()) p[tg_api::input_field_placeholder].escape(placeholder);
    }
};

}  // namespace fb