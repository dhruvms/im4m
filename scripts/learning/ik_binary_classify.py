import copy
import sys
import matplotlib.pyplot as plt

import numpy as np
import pandas as pd

import torch
import torch.nn as nn
import torch.optim as optim
import tqdm
from sklearn.metrics import roc_curve
from sklearn.model_selection import train_test_split

from models import BCNet
from helpers import process_data, draw_object, draw_base_rect, get_euler_from_R_tensor
from constants import *

# Helper function to train one model
def model_train(model, X_train, y_train, X_val, y_val):
	model = model.to(DEVICE)
	# loss function and optimizer
	loss_fn = nn.BCELoss()  # binary cross entropy
	optimizer = optim.Adam(model.parameters(), lr=0.0001)

	N = X_train.shape[0]
	n_epochs = 1000   # number of epochs to run
	batch_size = 1<<(int(np.sqrt(N))-1).bit_length()  # size of each batch
	batch_start = torch.arange(0, len(X_train), batch_size)

	# Hold the best model
	best_acc = -np.inf   # init to negative infinity
	best_weights = None
	best_acc_update_epoch = -1

	for epoch in range(n_epochs):
		idxs = torch.tensor(np.random.permutation(np.arange(0, N)))
		model.train()
		with tqdm.tqdm(batch_start, unit="batch", mininterval=0, disable=epoch % 50 != 0) as bar:
			bar.set_description("Epoch {}".format(epoch))
			for start in bar:
				batch_idxs = idxs[start:start+batch_size]
				# take a batch
				X_batch = X_train[batch_idxs]
				y_batch = y_train[batch_idxs]

				# forward pass
				y_pred = model(X_batch)
				loss = loss_fn(y_pred, y_batch)

				# backward pass
				optimizer.zero_grad()
				loss.backward()

				# update weights
				optimizer.step()
				# print progress

				acc = (y_pred.round() == y_batch).float().mean()
				bar.set_postfix(
					loss=float(loss),
					acc=float(acc)
				)
		# evaluate accuracy at end of each epoch
		model.eval()
		y_pred = model(X_val)
		acc = (y_pred.round() == y_val).float().mean()
		acc = float(acc)
		if acc > best_acc:
			# print('\tAccuracy improved at epoch {}'.format(epoch))
			best_acc = acc
			best_weights = copy.deepcopy(model.state_dict())
			best_acc_update_epoch = epoch
		elif ((best_acc_update_epoch >= 0) and epoch > best_acc_update_epoch + n_epochs//10):
			print('Training stopped at epoch {}. Last acc update was at epoch {}.'.format(epoch, best_acc_update_epoch))
			break

	# restore model and return best accuracy
	model.load_state_dict(best_weights)
	return best_acc

def get_yaw_from_R(R):
	return np.arctan2(R[1,0], R[0,0])

def make_rotation_matrix(rotvec):
	return np.reshape(rotvec, (3, 3)).transpose()

if __name__ == '__main__':
	all_data = process_data()

	# get training data - IK
	# ipdb> p X.columns.tolist()
	# ['o_ox', 'o_oy', 'o_oz', 'o_oyaw', 'o_shape', 'o_xs', 'o_ys', 'o_zs', 's_x', 's_y', 's_z', 's_yaw', 'm_dir_des', 'm_dist_des']

	X = copy.deepcopy(all_data.iloc[:, :24])
	# extract only yaw angle from rotation matrices of poses
	X_rot = X.iloc[:, 13:22].values.reshape(X.shape[0], 3, 3).transpose(0, 2, 1)
	_, _, yaws = get_euler_from_R_tensor(X_rot)
	X = X.drop(['o_mass', 'o_mu', 's_r11', 's_r21', 's_r31', 's_r12', 's_r22', 's_r32', 's_r13', 's_r23', 's_r33'], axis=1)
	X.insert(loc=11, column='s_yaw', value= yaws)
	# make binary labels
	y = copy.deepcopy(all_data.iloc[:, -1])
	y.loc[y > 0] = 2 # push IK failed => temporarily class 2
	y.loc[y <= 0] = 1 # push IK succeeded => class 1
	y.loc[y == 2] = 0 # push IK failed => class 0
	# Convert to 2D PyTorch tensors
	X = torch.tensor(X.values, dtype=torch.float32).to(DEVICE)
	y = torch.tensor(y, dtype=torch.float32).reshape(-1, 1).to(DEVICE)
	# train-test split: Hold out the test set for final model evaluation
	X_train, X_test, y_train, y_test = train_test_split(X, y, train_size=0.85, shuffle=True)

	# network params
	in_dim = X.shape[1]
	out_dim = 1
	# layers = 4
	h_sizes = [
				[32, 32, 32],
				[256, 256, 256],
				[32, 256, 32],
				[128, 64, 32],
				[64, 32, 64]
			]
	activation = 'relu'

	figure = plt.figure(figsize=(15,15))
	for H in range(len(h_sizes)):
		print("Train model: [{}]".format(', '.join(str(x) for x in h_sizes[H])))

		layers = len(h_sizes[H]) + 1
		model1 = BCNet()
		model1.initialise(in_dim, out_dim, activation=activation, layers=layers, h_sizes=h_sizes[H])
		acc = model_train(model1, X_train, y_train, X_test, y_test)
		# print(model1)
		print("Final model1 accuracy (4x32 deep, relu): {:.2f}%", acc*100)

		model1.eval()

		h = 0.01
		xx, yy = np.meshgrid(np.arange(-TABLE_SIZE[0], TABLE_SIZE[0], h),
							 np.arange(-TABLE_SIZE[1], TABLE_SIZE[1], h))
		push_to_xy = np.c_[xx.ravel(), yy.ravel()]
		num_test_pts = push_to_xy.shape[0]

		test_plots = 5
		with torch.no_grad():
			plot_count = 1
			axes = []
			for t in range(test_plots * test_plots):
				pidx = np.random.randint(X_test.shape[0])
				test_pose = X_test[pidx, :-2].cpu().numpy()[None, :]
				in_pts = np.repeat(test_pose, num_test_pts, axis=0)

				# dirs = np.arctan2(push_to_xy[:, 1] - in_pts[:, 1], push_to_xy[:, 0] - in_pts[:, 0])
				dirs = np.arctan2(push_to_xy[:, 1] - in_pts[:, 1], push_to_xy[:, 0] - in_pts[:, 0])
				dists = np.linalg.norm(in_pts[:, :2] - push_to_xy, axis=1)

				in_pts = np.hstack([in_pts, dirs[:, None], dists[:, None]])
				in_pts = torch.tensor(in_pts, dtype=torch.float32).to(DEVICE)

				preds = model1(in_pts)
				preds = preds.cpu().numpy()
				preds = preds.reshape(xx.shape)

				ax = plt.subplot(test_plots + 1, test_plots, plot_count)
				axes.append(ax)
				plot_count += 1

				draw_base_rect(ax)
				obj_draw = in_pts[0, :8].unsqueeze(0).cpu().numpy()
				draw_object(ax, obj_draw)

				asize = 0.08
				ayaw = test_pose[0, 11]
				ax.arrow(test_pose[0, 8], test_pose[0, 9], asize * np.cos(ayaw), asize * np.sin(ayaw),
							length_includes_head=True, head_width=0.02, head_length=0.02,
							ec='gold', fc='gold', alpha=0.8)
				cb = ax.contourf(xx, yy, preds, cmap=plt.cm.Greens, alpha=.8)

				ax.set_xticks(())
				ax.set_yticks(())
				ax.set_aspect('equal')
			# ax.axis('equal')
			# plt.gca().set_aspect('equal')
			# plt.colorbar(cb)
			plt.tight_layout()
			# plt.show()
			plt.savefig('[{}]'.format(','.join(str(x) for x in h_sizes[H])) + '-ikmap.png', bbox_inches='tight')
			[ax.cla() for ax in axes]
