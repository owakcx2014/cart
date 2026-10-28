#!/bin/bash

echo "📦 Installing build dependencies..."
sudo apt update
sudo apt install -y build-essential libgtk-3-dev pkg-config

echo "🔨 Compiling Cart..."
gcc cart.c -o cart $(pkg-config --cflags --libs gtk+-3.0)

if [ -f "cart" ]; then
    echo "🚀 Installing Cart to system..."
    
    # نسخ الملف التنفيذي مع صلاحيات التنفيذ وإنشاء المسار إن لم يكن موجوداً
    sudo install -Dm755 cart /usr/local/bin/cart
    
    # نسخ الأيقونة وموارد الـ assets إلى مسار النظام العام
    if [ -f "assets/folder-template.svg" ]; then
        sudo install -Dm644 assets/folder-template.svg /usr/local/share/cart/assets/folder-template.svg
    fi

    echo "✨ Cart installed successfully! You can now run it by typing 'cart' in your terminal."
else
    echo "❌ Compilation failed. Please check for errors."
    exit 1
fi
