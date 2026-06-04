#!/bin/bash

# 批量将视频帧文件夹转换为MJPEG编码的mp4视频
# 使用方法: ./convert_to_mjpeg.sh

# 获取脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 输出文件夹
OUTPUT_DIR="$SCRIPT_DIR/videos"
mkdir -p "$OUTPUT_DIR"

# 统计成功/失败
SUCCESS=0
FAILED=0

# 遍历所有子文件夹
for dir in "$SCRIPT_DIR"/*/; do
    # 跳过videos输出文件夹
    if [ "$dir" = "$OUTPUT_DIR/" ]; then
        continue
    fi

    dir_name=$(basename "$dir")
    manifest_file="$dir/manifest.json"

    # 检查是否有manifest.json文件
    if [ ! -f "$manifest_file" ]; then
        echo "⚠️  跳过 $dir_name: 未找到 manifest.json"
        continue
    fi

    # 从manifest.json读取参数
    fps=$(grep -o '"fps": *[0-9]*' "$manifest_file" | grep -o '[0-9]*')
    width=$(grep -o '"width": *[0-9]*' "$manifest_file" | grep -o '[0-9]*')
    height=$(grep -o '"height": *[0-9]*' "$manifest_file" | grep -o '[0-9]*')

    # 默认值
    fps=${fps:-24}
    width=${width:-480}
    height=${height:-800}

    output_file="$OUTPUT_DIR/${dir_name}.mp4"

    echo "========================================"
    echo "🎬 正在转换: $dir_name (MJPEG编码)"
    echo "   帧率: ${fps}fps, 分辨率: ${width}x${height}"

    # 执行ffmpeg转换 - MJPEG编码
    if ffmpeg -y -framerate "$fps" -i "$dir/frame_%05d.jpg" \
        -c:v mjpeg -q:v 2 -pix_fmt yuvj420p -vf "scale=${width}:${height}" \
        "$output_file" 2>/dev/null; then
        echo "✅ 成功: ${dir_name}.mp4"
        ((SUCCESS++))
    else
        echo "❌ 失败: $dir_name"
        ((FAILED++))
    fi
done

echo ""
echo "========================================"
echo "📊 批量转换完成! (MJPEG编码)"
echo "   成功: $SUCCESS 个"
echo "   失败: $FAILED 个"
echo "   输出目录: $OUTPUT_DIR"
echo "========================================"
