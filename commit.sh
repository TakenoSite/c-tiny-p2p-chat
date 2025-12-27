#!/bin/bash

# GitHubへコミットするシェルスクリプト
# 使用方法: ./git_commit.sh "コミットメッセージ"

# 引数チェック
if [ $# -eq 0 ]; then
    echo "エラー: コミットメッセージを引数として指定してください"
    echo "使用例: ./git_commit.sh \"コミットメッセージ\""
    exit 1
fi

COMMIT_MSG="$1"

# カレントディレクトリがGitリポジトリか確認
if ! git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
    echo "エラー: このディレクトリはGitリポジトリではありません"
    exit 1
fi

# ステータスチェック
if [ -z "$(git status --porcelain)" ]; then
    echo "変更がないため、コミットは行われません"
    exit 0
fi

# Git操作
echo "変更をステージングします..."
git add .

echo "コミットします..."
git commit -m "$COMMIT_MSG"

echo "リモートリポジトリにプッシュします..."
git push

echo "完了しました！"
